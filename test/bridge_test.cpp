// Test du pont interface <-> contrôleur (sans WebView2) : messages JS, paramètres, trames visuelles.
#include "editor_bridge.h"
#include <cstdio>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace mimic;

static int g_fail = 0;
#define CHECK(c, msg) do { bool ok_ = (c); std::printf ("  [%s] %s\n", ok_ ? "OK " : "ERR", msg); if (!ok_) ++g_fail; } while (0)

int main ()
{
	std::printf ("Pont d'interface\n");
	auto* ctl = new mimic::Controller;
	ctl->initialize (nullptr);
	std::vector<std::string> sent;
	{
		mimic::EditorBridge br (ctl, [&] (const std::string& m) { sent.push_back (m); });

		br.onWebMessage ("{\"t\":\"ready\"}");
		CHECK (sent.size () == 10, "ready : les 10 paramètres sont envoyés à la page");
		CHECK (sent[0].find ("\"id\":100") != std::string::npos && sent[0].find ("\"v\":0.62") != std::string::npos, "valeur par défaut MORPH = 0.62");

		sent.clear ();
		br.onWebMessage ("{\"t\":\"begin\",\"id\":101}");
		br.onWebMessage ("{\"t\":\"set\",\"id\":101,\"v\":0.75}");
		br.onWebMessage ("{\"t\":\"end\",\"id\":101}");
		CHECK (std::abs (ctl->getParamNormalized (kParamCarve) - 0.75) < 1e-9, "geste de la page : CARVE passe à 0.75 dans le contrôleur");
		CHECK (sent.empty (), "pas d'écho vers la page pendant le geste");

		br.onWebMessage ("{\"t\":\"set\",\"id\":999,\"v\":0.5}");
		br.onWebMessage ("{\"t\":\"set\",\"id\":100,\"v\":7}");
		CHECK (ctl->getParamNormalized (kParamMorph) == 1.0, "valeur hors plage bornée à 1");
		CHECK (sent.empty (), "identifiant inconnu ignoré");

		ctl->setParamNormalized (kParamGroove, 0.2); // automation hôte
		CHECK (sent.size () == 1 && sent[0].find ("\"id\":102") != std::string::npos, "automation hôte : la page est prévenue");

		ctl->setParamNormalized (kMeterInL, 0.3); // paramètre non-UI
		CHECK (sent.size () == 1, "vumètres internes non relayés comme paramètres");

		br.onWebMessage ("{\"t\":\"set\",\"id\":105,\"v\":1}");
		CHECK (ctl->getParamNormalized (kParamFreeze) == 1.0, "FREEZE depuis la page");

		sent.clear ();
		br.tick ();
		CHECK (sent.empty (), "tick sans trame : rien à envoyer");
		mimic::dsp::VisFrame f; f.seq = 5; f.energy = 0.5f; f.src[3] = 0.25f; f.synth[63] = 0.75f; f.outPk[1] = 0.4f;
		const_cast<mimic::VisMailbox&> (ctl->visual ()).publish (f);
		br.tick (); br.tick ();
		CHECK (sent.size () == 1, "tick : une trame envoyée une seule fois");
		const std::string& v = sent[0];
		CHECK (v.rfind ("{\"t\":\"vis\"", 0) == 0 && v.find ("\"e\":0.500") != std::string::npos && v.find ("\"o\":[0.000,0.400]") != std::string::npos, "contenu de la trame");
		int commas = 0; size_t a = v.find ("\"s\":["), b = v.find ("]", a);
		for (size_t i = a; i < b; ++i) commas += v[i] == ',';
		CHECK (commas == 63, "64 bandes source");

		std::string s; double d = 0;
		CHECK (mimic::jsonString ("{\"t\":\"a\\\"b\"}", "t", s) && s == "a\"b", "JSON : chaîne avec guillemet échappé");
		CHECK (mimic::jsonNumber ("{ \"v\" : -1.5e0 }", "v", d) && d == -1.5, "JSON : nombre avec espaces et exposant");
	}
	ctl->setParamNormalized (kParamWet, 0.5); // pont détruit : ne doit pas planter
	CHECK (true, "contrôleur utilisable après destruction du pont");
	ctl->terminate ();
	ctl->release ();
	std::printf (g_fail ? "\n%d ECHEC(S)\n" : "\nTOUS LES TESTS PASSENT\n", g_fail);
	return g_fail ? 1 : 0;
}
