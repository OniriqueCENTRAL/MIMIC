#include "controller.h"
#include "editor.h"
#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ustring.h"
#include "base/source/fstring.h"
#include "public.sdk/source/vst/vstparameters.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace mimic {

namespace {

// Affichage identique à l'interface : "62 %"
class PercentParam : public Parameter
{
public:
	PercentParam (const TChar* title, ParamID id, double def)
	: Parameter (title, id, STR16 ("%"), def, 0, ParameterInfo::kCanAutomate) {}
	void toString (ParamValue v, String128 s) const override
	{
		char t[32];
		std::snprintf (t, sizeof t, "%d", static_cast<int> (std::lround (v * 100.0)));
		UString (s, 128).fromAscii (t);
	}
	bool fromString (const TChar* s, ParamValue& v) const override
	{
		char buf[64] = {};
		UString128 u (s);
		u.toAscii (buf, 63);
		double d = 0;
		if (std::sscanf (buf, "%lf", &d) != 1)
			return false;
		v = std::min (1.0, std::max (0.0, d / 100.0));
		return true;
	}
};

// CARVE : "−9.1 dB" (0 .. −24 dB)
class CarveParam : public Parameter
{
public:
	CarveParam (ParamID id, double def) : Parameter (STR16 ("Carve"), id, STR16 ("dB"), def, 0, ParameterInfo::kCanAutomate) {}
	void toString (ParamValue v, String128 s) const override
	{
		char t[32];
		std::snprintf (t, sizeof t, "%.1f", dsp::carveDb (static_cast<float> (v)));
		UString (s, 128).fromAscii (t);
	}
	bool fromString (const TChar* s, ParamValue& v) const override
	{
		char buf[64] = {};
		UString128 u (s);
		u.toAscii (buf, 63);
		double d = 0;
		if (std::sscanf (buf, "%lf", &d) != 1)
			return false;
		v = std::min (1.0, std::max (0.0, -d / 24.0));
		return true;
	}
};

// OUTPUT : −48 .. +6 dB
class OutputParam : public Parameter
{
public:
	OutputParam (ParamID id, double def) : Parameter (STR16 ("Output"), id, STR16 ("dB"), def, 0, ParameterInfo::kCanAutomate) {}
	void toString (ParamValue v, String128 s) const override
	{
		char t[32];
		std::snprintf (t, sizeof t, "%+.1f", dsp::outputDb (static_cast<float> (v)));
		UString (s, 128).fromAscii (t);
	}
	bool fromString (const TChar* s, ParamValue& v) const override
	{
		char buf[64] = {};
		UString128 u (s);
		u.toAscii (buf, 63);
		double d = 0;
		if (std::sscanf (buf, "%lf", &d) != 1)
			return false;
		v = std::min (1.0, std::max (0.0, (d + 48.0) / 54.0));
		return true;
	}
};

} // namespace

tresult PLUGIN_API Controller::initialize (FUnknown* context)
{
	const tresult r = EditController::initialize (context);
	if (r != kResultTrue)
		return r;

	dsp::ParamValues d;
	parameters.addParameter (new PercentParam (STR16 ("Morph"), kParamMorph, d.morph));
	parameters.addParameter (new CarveParam (kParamCarve, d.carve));
	parameters.addParameter (new PercentParam (STR16 ("Groove"), kParamGroove, d.groove));
	parameters.addParameter (new PercentParam (STR16 ("Dry/Wet"), kParamWet, d.wet));
	parameters.addParameter (new OutputParam (kParamOutput, d.output));
	auto* frz = new StringListParameter (STR16 ("Lock/Freeze"), kParamFreeze);
	frz->appendString (STR16 ("Off"));
	frz->appendString (STR16 ("On"));
	parameters.addParameter (frz);

	auto* src = new StringListParameter (STR16 ("Source"), kParamSource);
	src->appendString (STR16 ("Voix"));
	src->appendString (STR16 ("Percu"));
	src->appendString (STR16 ("Texture"));
	parameters.addParameter (src);

	auto* inp = new StringListParameter (STR16 ("Input"), kParamInput);
	inp->appendString (STR16 ("Sidechain"));
	inp->appendString (STR16 ("Fichier"));
	parameters.addParameter (inp);

	auto* wav = new StringListParameter (STR16 ("Wave"), kParamWave);
	wav->appendString (STR16 ("Saw"));
	wav->appendString (STR16 ("Square"));
	wav->appendString (STR16 ("Triangle"));
	wav->appendString (STR16 ("Sine"));
	parameters.addParameter (wav);
	parameters.addParameter (new PercentParam (STR16 ("Blend"), kParamBlend, d.blend));

	const std::pair<ParamID, const TChar*> meters[] = {
	    {kMeterInL, STR16 ("In L")}, {kMeterInR, STR16 ("In R")}, {kMeterOutL, STR16 ("Out L")}, {kMeterOutR, STR16 ("Out R")}, {kMeterClip, STR16 ("Clip")}};
	for (const auto& [id, name] : meters)
		parameters.addParameter (name, nullptr, 0, 0, ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden, id);
	return kResultTrue;
}

tresult PLUGIN_API Controller::terminate () { return EditController::terminate (); }

tresult PLUGIN_API Controller::notify (IMessage* message)
{
	if (exchange_.onMessage (message))
		return kResultTrue;
	if (message && !std::strcmp (message->getMessageID (), kMsgClipInfo))
	{
		IAttributeList* a = message->getAttributes ();
		int64 ok = 0;
		a->getInt ("ok", ok);
		clip_ = ClipInfo {};
		if (ok)
		{
			TChar buf[512] = {};
			double sec = 0;
			const void* pk = nullptr;
			uint32 pkSize = 0;
			if (a->getString ("name", buf, sizeof (buf)) == kResultTrue)
				{ String n (buf); n.toMultiByte (kCP_Utf8); clip_.name = n.text8 (); }
			clip_.loaded = true;
			if (a->getFloat ("seconds", sec) == kResultTrue)
				clip_.seconds = sec;
			if (a->getBinary ("peaks", pk, pkSize) == kResultTrue && pkSize == sizeof (clip_.peaks))
				std::memcpy (clip_.peaks, pk, pkSize);
			// un fichier déposé devient la source d'analyse
			beginEdit (kParamInput);
			setParamNormalized (kParamInput, 1.0);
			performEdit (kParamInput, 1.0);
			endEdit (kParamInput);
		}
		else
		{
			TChar buf[512] = {};
			if (a->getString ("error", buf, sizeof (buf)) == kResultTrue)
				{ String n (buf); n.toMultiByte (kCP_Utf8); clip_.error = n.text8 (); }
		}
		return kResultTrue;
	}
	return EditController::notify (message);
}

tresult PLUGIN_API Controller::setComponentState (IBStream* state)
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
	setParamNormalized (kParamMorph, v[0]);
	setParamNormalized (kParamCarve, v[1]);
	setParamNormalized (kParamGroove, v[2]);
	setParamNormalized (kParamWet, v[3]);
	setParamNormalized (kParamOutput, v[4]);
	setParamNormalized (kParamFreeze, fr ? 1.0 : 0.0);
	setParamNormalized (kParamSource, (src + 0.5) / 3.0);
	int32 mode = 0;
	if (ver >= 2 && s.readInt32 (mode))
		setParamNormalized (kParamInput, mode ? 1.0 : 0.0);
	if (ver >= 3)
	{
		int32 w = 0;
		float b = 1.f;
		if (s.readInt32 (w) && s.readFloat (b))
		{
			setParamNormalized (kParamWave, std::min<int32> (3, std::max<int32> (0, w)) / 3.0);
			setParamNormalized (kParamBlend, b);
		}
	}
	return kResultTrue;
}

void Controller::loadFile (const std::string& utf8Path)
{
	if (IPtr<IMessage> msg = owned (allocateMessage ()))
	{
		msg->setMessageID (kMsgLoadFile);
		String w;
		w.assign (utf8Path.c_str ());
		w.toWideString (kCP_Utf8);
		msg->getAttributes ()->setString ("path", reinterpret_cast<const TChar*> (w.text16 ()));
		sendMessage (msg);
	}
	if (utf8Path.empty ())
	{
		clip_ = ClipInfo {};
		beginEdit (kParamInput);
		setParamNormalized (kParamInput, 0.0);
		performEdit (kParamInput, 0.0);
		endEdit (kParamInput);
	}
}

IPlugView* PLUGIN_API Controller::createView (FIDString name)
{
	if (name && std::strcmp (name, ViewType::kEditor) == 0)
		return createMimicEditor (this); // nullptr si la plate-forme n'a pas (encore) d'éditeur
	return nullptr;
}

tresult PLUGIN_API Controller::setParamNormalized (ParamID tag, ParamValue value)
{
	const tresult r = EditController::setParamNormalized (tag, value);
	if (observer_)
		observer_->onParam (tag, value);
	return r;
}

void PLUGIN_API Controller::queueOpened (DataExchangeUserContextID id, uint32, TBool& onBg)
{
	onBg = false; // réception sur le thread principal : la boîte aux lettres est atomique de toute façon
}
void PLUGIN_API Controller::queueClosed (DataExchangeUserContextID) {}

void PLUGIN_API Controller::onDataExchangeBlocksReceived (DataExchangeUserContextID id, uint32 numBlocks, DataExchangeBlock* blocks, TBool)
{
	if (id != kVisContextID)
		return;
	for (uint32 i = 0; i < numBlocks; ++i)
		if (blocks[i].size >= sizeof (dsp::VisFrame))
			mailbox_.publish (*reinterpret_cast<const dsp::VisFrame*> (blocks[i].data));
}

} // namespace mimic
