#pragma once
#include "dsp/fileplayer.h"
#include "dsp/mimic_dsp.h"
#include "ids.h"
#include "public.sdk/source/vst/utility/dataexchange.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace mimic {

class Processor : public Steinberg::Vst::AudioEffect
{
public:
	Processor ();
	static Steinberg::FUnknown* createInstance (void*) { return static_cast<Steinberg::Vst::IAudioProcessor*> (new Processor); }

	Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) override;
	Steinberg::tresult PLUGIN_API connect (Steinberg::Vst::IConnectionPoint* other) override;
	Steinberg::tresult PLUGIN_API disconnect (Steinberg::Vst::IConnectionPoint* other) override;
	Steinberg::tresult PLUGIN_API setBusArrangements (Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
	                                                  Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override;
	Steinberg::tresult PLUGIN_API canProcessSampleSize (Steinberg::int32 symbolicSampleSize) override;
	Steinberg::tresult PLUGIN_API setupProcessing (Steinberg::Vst::ProcessSetup& setup) override;
	Steinberg::tresult PLUGIN_API setActive (Steinberg::TBool state) override;
	Steinberg::uint32 PLUGIN_API getLatencySamples () override;
	Steinberg::tresult PLUGIN_API process (Steinberg::Vst::ProcessData& data) override;
	Steinberg::tresult PLUGIN_API notify (Steinberg::Vst::IMessage* message) override;
	Steinberg::tresult PLUGIN_API terminate () override;
	Steinberg::tresult PLUGIN_API setState (Steinberg::IBStream* state) override;
	Steinberg::tresult PLUGIN_API getState (Steinberg::IBStream* state) override;

private:
	static constexpr int kMaxEvents = 512;
	void handleParameters (Steinberg::Vst::IParameterChanges* changes);
	bool loadClip (const std::string& utf8Path, bool report);
	void reportClip (const dsp::AudioClip* c, const std::string& error);
	void applyPendingClip ();
	void sendVisual (const mimic::dsp::VisFrame& f);

	mimic::dsp::Engine engine_;
	mimic::dsp::ParamValues params_;
	std::array<mimic::dsp::NoteEvent, kMaxEvents> events_ {};
	Steinberg::Vst::DataExchangeHandler exchange_;
	// --- source fichier (chargée sur le thread principal, publiée sans verrou au thread audio) ---
	std::atomic<dsp::AudioClip*> pendingClip_ {nullptr};
	std::atomic<bool> hasPending_ {false};
	std::atomic<dsp::AudioClip*> retiredClip_ {nullptr};
	dsp::AudioClip* activeClip_ = nullptr; // propriété du thread audio, retiré via retiredClip_
	dsp::FilePlayer player_;
	std::vector<float> scratchL_, scratchR_;
	std::array<dsp::NoteEvent, kMaxEvents> chunkEvents_ {};
	std::string filePath_;     // sauvegardé dans l'état du projet
	int inputMode_ = 0;
	void freeClips ();
	double sampleRate_ = 48000.0;
	int visCounter_ = 0, visInterval_ = 800;
	std::uint32_t visSeq_ = 0;
	bool prepared_ = false;
};

} // namespace mimic
