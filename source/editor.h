#pragma once
#include "pluginterfaces/gui/iplugview.h"

namespace mimic {
class Controller;
// Crée l'éditeur graphique (fenêtre intégrée affichant l'interface MIMIC). nullptr si indisponible.
Steinberg::IPlugView* createMimicEditor (Controller* controller);

// Taille logique de l'interface (largeur x hauteur) : rapport d'aspect conservé au redimensionnement.
constexpr int kEditorWidth = 943;
constexpr int kEditorHeight = 512;
} // namespace mimic
