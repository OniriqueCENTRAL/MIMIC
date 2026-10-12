// MIMIC — pont entre l'interface web (JS) et le contrôleur VST3. Indépendant de la plate-forme et de WebView2 : testable seul.
//   JS -> plugin : {"t":"ready"} | {"t":"begin","id":N} | {"t":"set","id":N,"v":X} | {"t":"end","id":N}
//   plugin -> JS : {"t":"vis",...} (≈60 Hz) | {"t":"param","id":N,"v":X}
#pragma once
#include "controller.h"
#include <functional>
#include <string>

namespace mimic {

class EditorBridge : public IParamObserver
{
public:
	using Sender = std::function<void (const std::string&)>;

	EditorBridge (Controller* c, Sender send);
	~EditorBridge () override;

	void onWebMessage (const std::string& json);   // message reçu de la page
	void onDroppedFile (const std::string& utf8);  // chemin résolu par l'hôte pour un fichier déposé
	void tick ();                                  // à appeler ≈60 Hz depuis le thread UI
	void onParam (Steinberg::Vst::ParamID id, double v) override;

	static std::string visJson (const dsp::VisFrame& f);
	static std::string paramJson (Steinberg::Vst::ParamID id, double v);

private:
	void sendAllParams ();
	Controller* c_;
	Sender send_;
	bool ready_ = false, userEdit_ = false;
	std::uint32_t lastSeq_ = 0;
};

// Extraction minimale de champs d'un objet JSON plat produit par notre propre page.
bool jsonString (const std::string& j, const char* key, std::string& out);
bool jsonNumber (const std::string& j, const char* key, double& out);

} // namespace mimic
