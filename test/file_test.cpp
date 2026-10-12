// Test de bout en bout de la source "fichier" : décodage -> Processor VST3 (sans hôte) -> comparaison avec le moteur de référence.
#include "audio_file.h"
#include "processor.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <fstream>

using namespace Steinberg;
using namespace Steinberg::Vst;

static int g_fail = 0;
#define CHECK(c, msg) do { bool ok_ = (c); std::printf ("  [%s] %s\n", ok_ ? "OK " : "ERR", msg); if (!ok_) ++g_fail; } while (0)

static IPtr<MemoryStream> makeState (int mode, const std::string& path)
{
	auto ms = owned (new MemoryStream);
	auto w32 = [&] (int32 v) { int32 n; ms->write (&v, 4, &n); };
	auto wf = [&] (float v) { int32 n; ms->write (&v, 4, &n); };
	w32 (2);
	for (float v : {0.62f, 0.38f, 0.5f, 0.75f, 0.889f}) wf (v);
	w32 (0); w32 (0); w32 (mode);
	w32 (static_cast<int32> (path.size ()));
	int32 n;
	if (!path.empty ()) ms->write (const_cast<char*> (path.data ()), static_cast<int32> (path.size ()), &n);
	int64 r; ms->seek (0, IBStream::kIBSeekSet, &r);
	return ms;
}

struct Rendered { std::vector<float> L, R; };

struct Note { size_t at; bool on; int pitch; };
static std::vector<Note> g_notes;
static std::vector<size_t> g_reactivate; // instants (échantillons) où l'hôte désactive puis réactive le plugin (arrêt/lecture du transport)
static Rendered runProcessor (const std::string& path, int mode, int maxBlock, int block, size_t total, bool noteOn = true)
{
	auto* p = new mimic::Processor;
	p->initialize (nullptr);
	ProcessSetup su {kRealtime, kSample32, maxBlock, 48000.0};
	p->setupProcessing (su);
	auto st = makeState (mode, path);
	p->setState (st);
	p->setActive (true);
	Rendered out;
	std::vector<float> l (block), r (block);
	EventList ev (16);
	for (size_t pos = 0; pos < total; pos += block)
	{
		ev.clear ();
		for (size_t ra : g_reactivate)
			if (ra >= pos && ra < pos + block) { p->setActive (false); p->setActive (true); }
		if (!g_notes.empty ())
			for (const Note& nt : g_notes)
				if (nt.at >= pos && nt.at < pos + block)
				{
					Event e {};
					e.type = nt.on ? Event::kNoteOnEvent : Event::kNoteOffEvent; e.sampleOffset = (int32) (nt.at - pos);
					if (nt.on) { e.noteOn.pitch = nt.pitch; e.noteOn.velocity = 0.9f; e.noteOn.channel = 0; e.noteOn.noteId = -1; }
					else { e.noteOff.pitch = nt.pitch; e.noteOff.channel = 0; e.noteOff.noteId = -1; }
					ev.addEvent (e);
				}
		if (g_notes.empty () && noteOn && pos == 0)
		{
			Event e {};
			e.type = Event::kNoteOnEvent; e.sampleOffset = 0; e.noteOn.pitch = 48; e.noteOn.velocity = 1.0f; e.noteOn.channel = 0; e.noteOn.noteId = -1;
			ev.addEvent (e);
			e.noteOn.pitch = 60; ev.addEvent (e);
		}
		float* chans[2] = {l.data (), r.data ()};
		AudioBusBuffers ob {}; ob.numChannels = 2; ob.channelBuffers32 = chans;
		ProcessData d {};
		d.processMode = kRealtime; d.symbolicSampleSize = kSample32; d.numSamples = block;
		d.numOutputs = 1; d.outputs = &ob; d.inputEvents = &ev;
		p->process (d);
		out.L.insert (out.L.end (), l.begin (), l.end ());
		out.R.insert (out.R.end (), r.begin (), r.end ());
	}
	p->setActive (false);
	p->terminate ();
	p->release ();
	return out;
}

int main (int argc, char** argv)
{
	const std::string wav = argc > 1 ? argv[1] : "demo/sidechain.wav";
	std::printf ("Source fichier\n");

	std::string err;
	auto clip = mimic::loadAudioClip (wav, err);
	CHECK (clip && clip->frames () == 384000 && clip->sampleRate == 48000.0, "WAV 16 bits stéréo décodé (384000 trames, 48 kHz)");
	CHECK (clip && clip->name == "sidechain.wav", "nom du fichier conservé");
	float pk = 0; if (clip) for (float v : clip->peaks) pk = std::max (pk, v);
	CHECK (pk > 0.05f, "profil de crêtes calculé pour l'affichage");

	{ std::ofstream g ("/tmp/mimic_garbage.bin", std::ios::binary); for (int i = 0; i < 4096; ++i) g.put (char (i * 7)); }
	std::string e2; CHECK (!mimic::loadAudioClip ("/tmp/mimic_garbage.bin", e2) && !e2.empty (), "fichier non audio refusé proprement");
	std::string e3; CHECK (!mimic::loadAudioClip ("/tmp/n_existe_pas.wav", e3) && !e3.empty (), "fichier absent refusé proprement");

	// Référence : moteur seul, sidechain = le fichier, deux notes dès l'échantillon 0.
	const size_t total = 192000; // 4 s
	mimic::dsp::Engine ref; ref.prepare (48000.0, 1024);
	ref.setParams (mimic::dsp::ParamValues {}); ref.setForceLive (true); // comme le Processor en mode Fichier
	std::vector<float> rl (total), rr (total);
	mimic::dsp::NoteEvent evs[2] = {{0, true, 48, 1.0f}, {0, true, 60, 1.0f}};
	ref.process (clip->l.data (), clip->r.data (), rl.data (), rr.data (), (int) total, evs, 2);

	auto diff = [&] (const Rendered& a) { double m = 0; for (size_t i = 0; i < total; ++i) m = std::max (m, (double) std::fabs (a.L[i] - rl[i])); return m; };
	auto energy = [&] (const std::vector<float>& x) { double s = 0; for (size_t i = total / 2; i < total; ++i) s += x[i] * x[i]; return std::sqrt (s / (total / 2)); };

	Rendered a = runProcessor (wav, 1, 512, 512, total);
	CHECK (diff (a) < 1e-4, "Processor en mode Fichier == moteur de référence nourri par le fichier (bloc 512)");
	CHECK (energy (a.L) > 1e-3, "sortie non silencieuse");

	Rendered b = runProcessor (wav, 1, 128, 1000, total / 1000 * 1000);
	double m = 0; for (size_t i = 0; i < b.L.size (); ++i) m = std::max (m, (double) std::fabs (b.L[i] - rl[i]));
	std::printf ("    écart max %.3g\n", m); CHECK (m < 1e-4, "bloc hôte (1000) > bloc maximal annoncé (128) : découpage correct");

	Rendered c = runProcessor (wav, 0, 512, 512, total); // mode sidechain, pas de sidechain branché
	CHECK (diff (c) > 1e-2, "mode Sidechain sans entrée ≠ mode Fichier");

	Rendered d = runProcessor ("", 1, 512, 512, total); // mode fichier, aucun fichier
	CHECK (energy (d.L) > 1e-3 && std::isfinite (energy (d.L)), "mode Fichier sans fichier : le synthé joue quand même");

	Rendered f = runProcessor ("/tmp/n_existe_pas.wav", 1, 512, 512, 48000);
	CHECK (f.L.size () >= 48000, "projet rouvert avec fichier introuvable : aucun plantage");

	// Rejouer une note : le fichier doit repartir du début à CHAQUE note, même si la précédente joue encore (pluck court)
	{
		std::printf ("Notes rejouées avec un fichier court\n");
		const std::string shot = argc > 2 ? argv[2] : "oneshot.wav";
		const size_t SRh = 48000;
		auto rmsOf = [&] (const Rendered& r, double a, double b) { double acc = 0; size_t i0 = (size_t) (a * SRh), i1 = (size_t) (b * SRh); for (size_t i = i0; i < i1; ++i) acc += r.L[i] * r.L[i]; return std::sqrt (acc / (i1 - i0)); };
		g_notes.clear ();
		// note 1 à 0 s, note 2 à 0.35 s (la 1ère est encore en train de jouer son fichier), note 3 à 3 s (fichier fini depuis longtemps)
		for (double t : {0.0, 0.35, 3.0}) { g_notes.push_back ({(size_t) (t * SRh), true, 60}); g_notes.push_back ({(size_t) ((t + 0.3) * SRh), false, 60}); }
		std::sort (g_notes.begin (), g_notes.end (), [] (const Note& a, const Note& b) { return a.at < b.at; });
		Rendered r1 = runProcessor (shot, 1, 512, 512, 8 * SRh);
		double a1 = rmsOf (r1, 0.05, 0.25), a2 = rmsOf (r1, 0.40, 0.60), a3 = rmsOf (r1, 3.05, 3.25);
		std::printf ("    niveaux 0.2 s après chaque note : %.4f  %.4f  %.4f\n", a1, a2, a3);
		CHECK (a2 > 0.7 * a1, "note jouée pendant que la précédente joue encore : le fichier repart du début tout de suite");
		CHECK (a3 > 0.7 * a1, "note rejouée longtemps après : même niveau");
		g_notes.clear ();

		// Lecture unique : plus de son une fois le fichier terminé, pas de boucle
		mimic::dsp::AudioClip c; c.sampleRate = 48000; c.l.assign (1000, 0.5f); c.r.assign (1000, 0.5f);
		mimic::dsp::FilePlayer pl; pl.setClip (&c);
		std::vector<float> L (3000), R (3000);
		pl.fill (L.data (), R.data (), 3000, 48000.0);
		CHECK (L[10] == 0.0f, "avant la première note : silence");
		pl.trigger (); pl.fill (L.data (), R.data (), 3000, 48000.0);
		CHECK (L[10] == 0.5f && L[900] == 0.5f && L[1500] == 0.0f && L[2999] == 0.0f, "lecture unique : s'arrête à la fin du fichier");
		pl.trigger (); pl.fill (L.data (), R.data (), 100, 48000.0);
		CHECK (L[0] == 0.5f, "trigger : repart du début");

		// Étirement : la hauteur suit la note, la durée reste la même
		{
			const double SRt = 48000.0; mimic::dsp::AudioClip s; s.sampleRate = SRt;
			s.l.resize (96000); s.r.resize (96000);
			for (size_t i = 0; i < s.l.size (); ++i) s.l[i] = s.r[i] = 0.5f * (float) std::sin (6.2831853 * 440.0 * i / SRt);
			mimic::dsp::FilePlayer p2; p2.setClip (&s);
			for (double ratio : { 0.5, 1.5, 2.0 })
			{
				p2.trigger (ratio);
				std::vector<float> a (110000), b (110000);
				p2.fill (a.data (), b.data (), 110000, SRt);
				size_t endIdx = 0; for (size_t i = 0; i < a.size (); ++i) if (std::fabs (a[i]) > 0.02f) endIdx = i;
				int zc = 0; for (size_t i = 40000; i + 1 < 56000; ++i) if (a[i] < 0 && a[i + 1] >= 0) ++zc;
				const double fq = zc / (16000.0 / SRt);
				std::printf ("    ratio %.1f : %.0f Hz\n", ratio, fq); CHECK (std::fabs (fq - 440.0 * ratio) < 0.04 * 440.0 * ratio, "hauteur suit le ratio");
				std::printf ("    fin à %zu / 96000\n", endIdx); CHECK (endIdx > 90000 && endIdx < 98000, "durée inchangée");
			}
		}
	}

	std::printf (g_fail ? "\n%d ECHEC(S)\n" : "\nTOUS LES TESTS PASSENT\n", g_fail);
	return g_fail ? 1 : 0;
}
