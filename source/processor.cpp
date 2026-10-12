#include "processor.h"
#include "audio_file.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "base/source/fstring.h"
#include "public.sdk/source/vst/vstaudioprocessoralgo.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/vst/vstaudioprocessoralgo.h"
#include "base/source/fstreamer.h"
#include <algorithm>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace mimic {

static constexpr int32 kStateVersion = 3;

Processor::Processor ()
: exchange_ (this, [this] (DataExchangeHandler::Config& cfg, const ProcessSetup& setup) {
	  cfg.blockSize = sizeof (dsp::VisFrame);
	  cfg.numBlocks = 8;
	  cfg.alignment = 16;
	  cfg.userContextID = kVisContextID;
	  visInterval_ = std::max (64, static_cast<int> (setup.sampleRate / 60.0));
	  return true;
  })
{
	setControllerClass (kControllerUID);
}

tresult PLUGIN_API Processor::initialize (FUnknown* context)
{
	const tresult r = AudioEffect::initialize (context);
	if (r != kResultTrue)
		return r;
	addEventInput (STR16 ("MIDI In"), 16);
	addAudioInput (STR16 ("Sidechain"), SpeakerArr::kStereo, BusTypes::kAux, BusInfo::kDefaultActive);
	addAudioOutput (STR16 ("Output"), SpeakerArr::kStereo);
	return kResultTrue;
}

tresult PLUGIN_API Processor::connect (IConnectionPoint* other)
{
	const tresult r = AudioEffect::connect (other);
	exchange_.onConnect (other, getHostContext ());
	return r;
}

tresult PLUGIN_API Processor::disconnect (IConnectionPoint* other)
{
	exchange_.onDisconnect (other);
	return AudioEffect::disconnect (other);
}

tresult PLUGIN_API Processor::setBusArrangements (SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts)
{
	if (numIns != 1 || numOuts != 1 || outputs[0] != SpeakerArr::kStereo)
		return kResultFalse;
	if (inputs[0] != SpeakerArr::kStereo && inputs[0] != SpeakerArr::kMono)
		return kResultFalse;
	return AudioEffect::setBusArrangements (inputs, numIns, outputs, numOuts);
}

tresult PLUGIN_API Processor::canProcessSampleSize (int32 s) { return s == kSample32 ? kResultTrue : kResultFalse; }

tresult PLUGIN_API Processor::setupProcessing (ProcessSetup& setup)
{
	sampleRate_ = setup.sampleRate;
	visInterval_ = std::max (64, static_cast<int> (sampleRate_ / 60.0));
	return AudioEffect::setupProcessing (setup);
}

tresult PLUGIN_API Processor::setActive (TBool state)
{
	if (state)
	{
		engine_.prepare (sampleRate_, 1024); // seule allocation : hors thread audio
		engine_.setParams (params_);
		const int cap = std::max (256, static_cast<int> (processSetup.maxSamplesPerBlock));
		scratchL_.assign (static_cast<size_t> (cap), 0.0f);
		scratchR_.assign (static_cast<size_t> (cap), 0.0f);
		if (!filePath_.empty () && !pendingClip_.load () && !activeClip_)
			loadClip (filePath_, false); // recharge après changement de fréquence / ouverture de projet
		prepared_ = true;
		visCounter_ = 0;
		exchange_.onActivate (processSetup);
	}
	else
	{
		exchange_.onDeactivate ();
		prepared_ = false;
	}
	return AudioEffect::setActive (state);
}

uint32 PLUGIN_API Processor::getLatencySamples () { return static_cast<uint32> (engine_.latencySamples ()); }

void Processor::handleParameters (IParameterChanges* changes)
{
	if (!changes)
		return;
	bool dirty = false;
	const int32 n = changes->getParameterCount ();
	for (int32 i = 0; i < n; ++i)
	{
		IParamValueQueue* q = changes->getParameterData (i);
		if (!q || q->getPointCount () <= 0)
			continue;
		ParamValue v;
		int32 off;
		if (q->getPoint (q->getPointCount () - 1, off, v) != kResultTrue)
			continue;
		const float f = static_cast<float> (std::min (1.0, std::max (0.0, v)));
		switch (q->getParameterId ())
		{
			case kParamMorph: params_.morph = f; dirty = true; break;
			case kParamCarve: params_.carve = f; dirty = true; break;
			case kParamGroove: params_.groove = f; dirty = true; break;
			case kParamWet: params_.wet = f; dirty = true; break;
			case kParamOutput: params_.output = f; dirty = true; break;
			case kParamFreeze: params_.freeze = f > 0.5f; dirty = true; break;
			case kParamInput: inputMode_ = f > 0.5f ? 1 : 0; break;
			case kParamSource: params_.source = std::min (2, static_cast<int> (f * 3.0f)); dirty = true; break;
			case kParamWave: params_.wave = std::min (3, static_cast<int> (f * 3.0f + 0.5f)); dirty = true; break;
			case kParamBlend: params_.blend = f; dirty = true; break;
			default: break;
		}
	}
	if (dirty)
		engine_.setParams (params_);
}

void Processor::sendVisual (const dsp::VisFrame& f)
{
	DataExchangeBlock b = exchange_.getCurrentOrNewBlock ();
	if (b.blockID == InvalidDataExchangeBlockID || b.size < sizeof (dsp::VisFrame))
		return; // file pleine : on saute une trame, jamais d'attente
	std::memcpy (b.data, &f, sizeof (dsp::VisFrame));
	reinterpret_cast<dsp::VisFrame*> (b.data)->seq = ++visSeq_;
	exchange_.sendCurrentBlock ();
}

tresult PLUGIN_API Processor::process (ProcessData& data)
{
	handleParameters (data.inputParameterChanges);

	if (data.numOutputs < 1 || data.numSamples <= 0 || !prepared_)
		return kResultTrue;
	AudioBusBuffers& out = data.outputs[0];
	if (out.numChannels < 2 || !out.channelBuffers32)
		return kResultTrue;

	// Événements MIDI (note on/off), triés par offset
	int nev = 0;
	if (data.inputEvents)
	{
		const int32 ne = data.inputEvents->getEventCount ();
		for (int32 i = 0; i < ne && nev < kMaxEvents; ++i)
		{
			Event e;
			if (data.inputEvents->getEvent (i, e) != kResultTrue)
				continue;
			if (e.type == Event::kNoteOnEvent)
			{
				const bool on = e.noteOn.velocity > 0.0f;
				events_[nev++] = {e.sampleOffset, on, e.noteOn.pitch, e.noteOn.velocity};
			}
			else if (e.type == Event::kNoteOffEvent)
				events_[nev++] = {e.sampleOffset, false, e.noteOff.pitch, 0.0f};
		}
		std::stable_sort (events_.begin (), events_.begin () + nev, [] (const dsp::NoteEvent& a, const dsp::NoteEvent& b) { return a.offset < b.offset; });
	}

	applyPendingClip ();

	// Sidechain hôte (bus aux) : null-safe, mono toléré
	const float* hostL = nullptr;
	const float* hostR = nullptr;
	if (data.numInputs > 0 && data.inputs[0].numChannels > 0 && data.inputs[0].channelBuffers32 && data.inputs[0].silenceFlags != 0x3 && !(data.inputs[0].numChannels == 1 && (data.inputs[0].silenceFlags & 1)))
	{
		hostL = data.inputs[0].channelBuffers32[0];
		hostR = data.inputs[0].numChannels > 1 ? data.inputs[0].channelBuffers32[1] : hostL;
	}

	if (hostL == out.channelBuffers32[0] || hostR == out.channelBuffers32[1] || hostL == out.channelBuffers32[1])
		hostL = hostR = nullptr; // entrée en place sur la sortie : l'écouter créerait une boucle de rétroaction

	const bool useFile = inputMode_ == 1 && player_.hasClip ();
	engine_.setForceLive (useFile);

	// Traitement par tranches (taille de bloc hôte arbitraire, scratch préalloué)
	const int cap = static_cast<int> (scratchL_.size ());
	int done = 0, ev0 = 0;
	while (done < data.numSamples)
	{
		int len = std::min (data.numSamples - done, cap);
		const float* scL = hostL ? hostL + done : nullptr;
		const float* scR = hostR ? hostR + done : nullptr;
		if (inputMode_ == 1)
		{
			if (useFile)
			{
				// Chaque nouvelle note relance le fichier depuis le début, à l'échantillon près (même si la précédente joue encore).
				for (int k = ev0; k < nev && events_[k].offset <= done; ++k)
					if (events_[k].on) // do = ratio de hauteur : C5 (MIDI 60) = fichier d'origine
						player_.trigger (std::pow (2.0, (std::min (84, std::max (36, static_cast<int> (events_[k].note))) - 60) / 12.0));
				for (int k = ev0; k < nev; ++k)
					if (events_[k].on && events_[k].offset > done)
					{
						len = std::min (len, events_[k].offset - done);
						break;
					}
				player_.fill (scratchL_.data (), scratchR_.data (), len, sampleRate_);
				scL = scratchL_.data ();
				scR = scratchR_.data ();
			}
			else
				scL = scR = nullptr; // mode fichier sans fichier : pas de modulateur
		}
		int nc = 0;
		while (ev0 < nev && events_[ev0].offset < done + len)
		{
			chunkEvents_[nc] = events_[ev0];
			chunkEvents_[nc].offset = std::max (0, events_[ev0].offset - done);
			++nc;
			++ev0;
		}
		engine_.process (scL, scR, out.channelBuffers32[0] + done, out.channelBuffers32[1] + done, len, chunkEvents_.data (), nc);
		done += len;
	}
	out.silenceFlags = 0;

	// Trame visuelle ~60 Hz
	visCounter_ += data.numSamples;
	if (visCounter_ >= visInterval_)
	{
		visCounter_ = 0;
		sendVisual (engine_.visual ());
	}

	// Vumètres -> hôte (paramètres de sortie, lecture seule)
	if (data.outputParameterChanges)
	{
		const dsp::VisFrame& v = engine_.visual ();
		const std::pair<ParamID, float> m[] = {{kMeterInL, v.inPk[0]}, {kMeterInR, v.inPk[1]}, {kMeterOutL, v.outPk[0]}, {kMeterOutR, v.outPk[1]}, {kMeterClip, v.clip}};
		for (const auto& [id, val] : m)
		{
			int32 idx;
			if (IParamValueQueue* q = data.outputParameterChanges->addParameterData (id, idx))
			{
				int32 pi;
				q->addPoint (0, std::min (1.0f, std::max (0.0f, val)), pi);
			}
		}
	}
	return kResultTrue;
}

tresult PLUGIN_API Processor::setState (IBStream* state)
{
	if (!state)
		return kResultFalse;
	IBStreamer s (state, kLittleEndian);
	int32 ver = 0;
	if (!s.readInt32 (ver) || ver < 1)
		return kResultFalse;
	float v[5];
	for (float& x : v)
		if (!s.readFloat (x))
			return kResultFalse;
	int32 fr = 0, src = 0;
	if (!s.readInt32 (fr) || !s.readInt32 (src))
		return kResultFalse;
	params_.morph = v[0]; params_.carve = v[1]; params_.groove = v[2]; params_.wet = v[3]; params_.output = v[4];
	params_.freeze = fr != 0;
	params_.source = std::min (2, std::max (0, static_cast<int> (src)));
	if (ver >= 2)
	{
		int32 mode = 0, len = 0;
		bool ok = s.readInt32 (mode);
		if (ok && ver >= 3)
		{
			int32 w = 0;
			float b = 1.0f;
			ok = s.readInt32 (w) && s.readFloat (b);
			params_.wave = std::min (3, std::max (0, static_cast<int> (w)));
			params_.blend = std::min (1.0f, std::max (0.0f, b));
		}
		if (ok && s.readInt32 (len) && len >= 0 && len < 4096)
		{
			inputMode_ = mode ? 1 : 0;
			std::string p (static_cast<size_t> (len), '\0');
			if (len == 0 || s.readRaw (p.data (), static_cast<TSize> (len)) == len)
			{
				filePath_ = p;
				if (!filePath_.empty ())
					loadClip (filePath_, true);
			}
		}
	}
	if (prepared_)
		engine_.setParams (params_);
	return kResultTrue;
}

tresult PLUGIN_API Processor::getState (IBStream* state)
{
	IBStreamer s (state, kLittleEndian);
	s.writeInt32 (kStateVersion);
	for (float x : {params_.morph, params_.carve, params_.groove, params_.wet, params_.output})
		s.writeFloat (x);
	s.writeInt32 (params_.freeze ? 1 : 0);
	s.writeInt32 (params_.source);
	s.writeInt32 (inputMode_);
	s.writeInt32 (params_.wave);
	s.writeFloat (params_.blend);
	s.writeInt32 (static_cast<int32> (filePath_.size ()));
	if (!filePath_.empty ())
		s.writeRaw (filePath_.data (), static_cast<TSize> (filePath_.size ()));
	return kResultTrue;
}


// ---------------------------------------------------------------------------------------------
// Fichier audio déposé
// ---------------------------------------------------------------------------------------------
void Processor::applyPendingClip ()
{
	if (!hasPending_.load (std::memory_order_acquire))
		return;
	hasPending_.store (false, std::memory_order_relaxed);
	dsp::AudioClip* next = pendingClip_.exchange (nullptr, std::memory_order_acq_rel);
	if (!next && !activeClip_)
		return;
	dsp::AudioClip* old = activeClip_;
	activeClip_ = next;
	player_.setClip (activeClip_);
	if (old)
		retiredClip_.store (old, std::memory_order_release); // libéré hors thread audio
}

void Processor::freeClips ()
{
	delete pendingClip_.exchange (nullptr);
	delete retiredClip_.exchange (nullptr);
}

void Processor::reportClip (const dsp::AudioClip* c, const std::string& error)
{
	if (IPtr<IMessage> msg = owned (allocateMessage ()))
	{
		msg->setMessageID (kMsgClipInfo);
		IAttributeList* a = msg->getAttributes ();
		a->setInt ("ok", c ? 1 : 0);
		if (c)
		{
			String name;
			name.assign (c->name.c_str ());
			name.toWideString (kCP_Utf8);
			a->setString ("name", reinterpret_cast<const TChar*> (name.text16 ()));
			a->setFloat ("seconds", c->seconds ());
			a->setBinary ("peaks", c->peaks, sizeof (c->peaks));
		}
		else
		{
			String e;
			e.assign (error.c_str ());
			e.toWideString (kCP_Utf8);
			a->setString ("error", reinterpret_cast<const TChar*> (e.text16 ()));
		}
		sendMessage (msg);
	}
}

// Thread principal uniquement (décodage = E/S disque). Publie le clip au thread audio sans verrou.
bool Processor::loadClip (const std::string& utf8Path, bool report)
{
	std::string err;
	std::unique_ptr<dsp::AudioClip> clip;
	if (!utf8Path.empty ())
		clip = loadAudioClip (std::filesystem::u8path (utf8Path), err);
	delete retiredClip_.exchange (nullptr); // le précédent clip retiré n'est plus lu par le thread audio
	if (!clip && !utf8Path.empty ())
	{
		if (report)
			reportClip (nullptr, err);
		return false;
	}
	dsp::AudioClip* raw = clip.release ();
	delete pendingClip_.exchange (raw, std::memory_order_acq_rel); // jamais vu par le thread audio
	hasPending_.store (true, std::memory_order_release);
	filePath_ = utf8Path;
	if (report && raw)
		reportClip (raw, "");
	return true;
}

tresult PLUGIN_API Processor::notify (IMessage* message)
{
	if (message && !std::strcmp (message->getMessageID (), kMsgLoadFile))
	{
		TChar buf[2048] = {};
		if (IAttributeList* a = message->getAttributes ())
			if (a->getString ("path", buf, sizeof (buf)) == kResultTrue)
			{
				String s (buf);
				s.toMultiByte (kCP_Utf8);
				loadClip (s.text8 (), true);
			}
		return kResultTrue;
	}
	return AudioEffect::notify (message);
}

tresult PLUGIN_API Processor::terminate ()
{
	freeClips ();
	delete activeClip_;
	activeClip_ = nullptr;
	return AudioEffect::terminate ();
}

} // namespace mimic
