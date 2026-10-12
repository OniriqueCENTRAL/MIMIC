// MIMIC — identifiants partagés processeur / contrôleur.
#pragma once
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstdataexchange.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace mimic {

enum ParamId : Steinberg::Vst::ParamID
{
	kParamMorph = 100,
	kParamCarve = 101,
	kParamGroove = 102,
	kParamWet = 103,
	kParamOutput = 104,
	kParamFreeze = 105,
	kParamSource = 106,
	kParamWave = 108,  // forme d'onde du synthé : 0 saw, 1 carré, 2 triangle, 3 sinus
	kParamBlend = 109, // 0 = son de base seul, 1 = synthé MIMIC seul
	kParamInput = 107, // 0 = sidechain hôte, 1 = fichier déposé dans la gueule
	// Paramètres de lecture seule (vumètres) renvoyés à l'hôte
	kMeterInL = 200,
	kMeterInR = 201,
	kMeterOutL = 202,
	kMeterOutR = 203,
	kMeterClip = 204,
};

// Messages contrôleur <-> processeur (fichier déposé)
static constexpr const char* kMsgLoadFile = "MimicLoadFile";   // contrôleur -> processeur : "path" (string16), vide = décharger
static constexpr const char* kMsgClipInfo = "MimicClipInfo";   // processeur -> contrôleur : "ok", "name", "seconds", "peaks", "error"

static constexpr Steinberg::Vst::DataExchangeUserContextID kVisContextID = 7;

static const Steinberg::FUID kProcessorUID (0x4D494D49, 0x43000001, 0x50415241, 0x53495445);
static const Steinberg::FUID kControllerUID (0x4D494D49, 0x43000002, 0x50415241, 0x53495445);

} // namespace mimic
