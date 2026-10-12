#include "controller.h"
#include "ids.h"
#include "processor.h"
#include "version.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

using namespace Steinberg;

BEGIN_FACTORY_DEF (stringCompanyName, stringCompanyWeb, stringCompanyEmail)

DEF_CLASS2 (INLINE_UID_FROM_FUID (mimic::kProcessorUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
            stringPluginName, Vst::kDistributable, Vst::PlugType::kInstrumentSynth, FULL_VERSION_STR,
            kVstVersionString, mimic::Processor::createInstance)

DEF_CLASS2 (INLINE_UID_FROM_FUID (mimic::kControllerUID), PClassInfo::kManyInstances, kVstComponentControllerClass,
            stringPluginName "Controller", 0, "", FULL_VERSION_STR, kVstVersionString,
            mimic::Controller::createInstance)

END_FACTORY
