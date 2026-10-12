// Plates-formes sans éditeur natif pour l'instant (Linux ; macOS viendra avec WKWebView).
#include "editor.h"
namespace mimic {
Steinberg::IPlugView* createMimicEditor (Controller*) { return nullptr; }
}
