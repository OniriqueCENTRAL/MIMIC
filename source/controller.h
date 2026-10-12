#pragma once
#include "dsp/engine.h"
#include "ids.h"
#include "public.sdk/source/vst/utility/dataexchange.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/vst/ivstdataexchange.h"
#include <atomic>
#include <string>

namespace mimic {

// Dernière trame visuelle reçue (lue par le futur éditeur graphique, thread UI).
class VisMailbox
{
public:
	void publish (const dsp::VisFrame& f)
	{
		const auto s = seq_.load (std::memory_order_relaxed);
		seq_.store (s + 1, std::memory_order_release); // impair = écriture en cours
		frame_ = f;
		seq_.store (s + 2, std::memory_order_release);
	}
	bool read (dsp::VisFrame& out) const
	{
		for (int tries = 0; tries < 4; ++tries)
		{
			const auto a = seq_.load (std::memory_order_acquire);
			if (a & 1u)
				continue;
			out = frame_;
			if (seq_.load (std::memory_order_acquire) == a)
				return true;
		}
		return false;
	}
private:
	std::atomic<std::uint32_t> seq_ {0};
	dsp::VisFrame frame_ {};
};

// Observateur des changements de paramètres (automation hôte, préréglages…) — thread principal.
struct IParamObserver
{
	virtual ~IParamObserver () = default;
	virtual void onParam (Steinberg::Vst::ParamID id, double normalized) = 0;
};

class Controller : public Steinberg::Vst::EditController, public Steinberg::Vst::IDataExchangeReceiver
{
public:
	static Steinberg::FUnknown* createInstance (void*) { return static_cast<Steinberg::Vst::IEditController*> (new Controller); }

	OBJ_METHODS (Controller, Steinberg::Vst::EditController)
	DEFINE_INTERFACES
		DEF_INTERFACE (Steinberg::Vst::IDataExchangeReceiver)
	END_DEFINE_INTERFACES (Steinberg::Vst::EditController)
	REFCOUNT_METHODS (Steinberg::Vst::EditController)

	Steinberg::tresult PLUGIN_API initialize (Steinberg::FUnknown* context) override;
	Steinberg::tresult PLUGIN_API terminate () override;
	Steinberg::tresult PLUGIN_API notify (Steinberg::Vst::IMessage* message) override;
	Steinberg::tresult PLUGIN_API setComponentState (Steinberg::IBStream* state) override;

	void PLUGIN_API queueOpened (Steinberg::Vst::DataExchangeUserContextID id, Steinberg::uint32 blockSize, Steinberg::TBool& onBg) override;
	void PLUGIN_API queueClosed (Steinberg::Vst::DataExchangeUserContextID id) override;
	void PLUGIN_API onDataExchangeBlocksReceived (Steinberg::Vst::DataExchangeUserContextID id, Steinberg::uint32 numBlocks,
	                                              Steinberg::Vst::DataExchangeBlock* blocks, Steinberg::TBool onBg) override;

	Steinberg::IPlugView* PLUGIN_API createView (Steinberg::FIDString name) override;
	Steinberg::tresult PLUGIN_API setParamNormalized (Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value) override;
	void setObserver (IParamObserver* o) { observer_ = o; }

	const VisMailbox& visual () const { return mailbox_; }

	// Fichier déposé dans la gueule (appelé par l'éditeur, thread principal). Chemin UTF-8.
	void loadFile (const std::string& utf8Path);
	void clearFile () { loadFile (std::string ()); }

	struct ClipInfo
	{
		bool loaded = false;
		std::string name, error;
		double seconds = 0.0;
		float peaks[256] = {};
	};
	const ClipInfo& clipInfo () const { return clip_; }

private:
	Steinberg::Vst::DataExchangeReceiverHandler exchange_ {this};
	VisMailbox mailbox_;
	ClipInfo clip_;
	IParamObserver* observer_ = nullptr;
};

} // namespace mimic
