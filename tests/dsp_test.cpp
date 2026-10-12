// MIMIC — tests du moteur DSP (sans SDK). Compile : voir CMakeLists (cible mimic_dsp_test).
#include "dsp/mimic_dsp.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>

using namespace mimic::dsp;

// ---- compteur d'allocations (vérifie qu'on ne alloue rien dans process())
static bool g_count = false;
static long g_allocs = 0;
void* operator new (std::size_t n)
{
	if (g_count) ++g_allocs;
	if (void* p = std::malloc (n)) return p;
	throw std::bad_alloc ();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

static const double SR = 48000.0;
static int g_fail = 0;
#define CHECK(cond, ...) do { bool ok_ = (cond); std::printf ("  [%s] ", ok_ ? "OK " : "ÉCHEC"); std::printf (__VA_ARGS__); std::printf ("\n"); if (!ok_) ++g_fail; } while (0)

// ---- signaux de test
struct Resonator { float a1 = 0, a2 = 0, y1 = 0, y2 = 0;
	void set (float f, float bw) { float r = std::exp (-kPi * bw / (float) SR); a1 = 2 * r * std::cos (kTwoPi * f / (float) SR); a2 = -r * r; }
	float tick (float x) { float y = x + a1 * y1 + a2 * y2; y2 = y1; y1 = y; return y; } };

// voyelle : train d'impulsions (f0) filtré par deux formants
static std::vector<float> vowel (float f1, float f2, float f0, double seconds, float amp = 0.5f)
{
	const int n = (int) (seconds * SR);
	std::vector<float> out ((size_t) n);
	Resonator r1, r2, r3; r1.set (f1, 90); r2.set (f2, 130); r3.set (3200, 250);
	float ph = 0;
	for (int i = 0; i < n; ++i)
	{
		float x = 0;
		ph += f0 / (float) SR;
		if (ph >= 1) { ph -= 1; x = 1; }
		out[(size_t) i] = amp * 0.02f * (r1.tick (x) + 0.7f * r2.tick (x) + 0.3f * r3.tick (x));
	}
	return out;
}

static std::vector<float> whiteNoise (double seconds, float amp, unsigned seed = 1)
{
	std::mt19937 g (seed); std::uniform_real_distribution<float> d (-1, 1);
	std::vector<float> o ((size_t) (seconds * SR));
	for (auto& v : o) v = amp * d (g);
	return o;
}

// spectre moyen (Welch, Hann 4096) -> énergie dans une bande
static double bandEnergyDb (const std::vector<float>& x, size_t from, size_t to, float f1, float f2)
{
	const int N = 4096; Fft fft; fft.init (N);
	std::vector<float> buf ((size_t) (2 * N));
	double acc = 0; int cnt = 0;
	for (size_t s = from; s + N <= to; s += N / 2)
	{
		for (int i = 0; i < N; ++i) { float w = 0.5f - 0.5f * std::cos (kTwoPi * i / N); buf[(size_t) (2 * i)] = x[s + (size_t) i] * w; buf[(size_t) (2 * i + 1)] = 0; }
		fft.forward (buf.data ());
		for (int k = 1; k < N / 2; ++k)
		{
			float f = (float) (k * SR / N);
			if (f >= f1 && f <= f2) acc += buf[(size_t) (2 * k)] * buf[(size_t) (2 * k)] + buf[(size_t) (2 * k + 1)] * buf[(size_t) (2 * k + 1)];
		}
		++cnt;
	}
	return 10.0 * std::log10 (acc / std::max (1, cnt) + 1e-20);
}

static double rms (const std::vector<float>& x, size_t a, size_t b)
{ double s = 0; for (size_t i = a; i < b; ++i) s += x[i] * x[i]; return std::sqrt (s / std::max<size_t> (1, b - a)); }

struct Run { std::vector<float> L, R; };

// rend `n` échantillons ; sc peut être vide ; notes = événements (offsets absolus)
static Run render (Engine& e, const std::vector<float>& sc, size_t n, const std::vector<NoteEvent>& notes, const std::vector<std::pair<size_t, ParamValues>>& changes = {})
{
	Run r; r.L.assign (n, 0); r.R.assign (n, 0);
	const int B = 256; size_t ci = 0, ni = 0;
	std::vector<NoteEvent> ev;
	for (size_t pos = 0; pos < n; pos += B)
	{
		int cnt = (int) std::min<size_t> (B, n - pos);
		while (ci < changes.size () && changes[ci].first <= pos) { e.setParams (changes[ci].second); ++ci; }
		ev.clear ();
		while (ni < notes.size () && notes[ni].offset < (int) (pos + cnt)) { NoteEvent x = notes[ni]; x.offset = std::max (0, x.offset - (int) pos); ev.push_back (x); ++ni; }
		const float* s = sc.empty () ? nullptr : sc.data () + pos;
		e.process (s, s, r.L.data () + pos, r.R.data () + pos, cnt, ev.data (), (int) ev.size ());
	}
	return r;
}

static NoteEvent on (int off, int note, float v = 1) { return { off, true, note, v }; }

static void writeWav (const char* path, const std::vector<float>& L, const std::vector<float>& R)
{
	FILE* f = std::fopen (path, "wb"); if (!f) return;
	uint32_t n = (uint32_t) L.size (), dataBytes = n * 4, riff = 36 + dataBytes, sr = (uint32_t) SR, br = sr * 4; uint16_t fmt = 1, ch = 2, ba = 4, bps = 16; uint32_t sz = 16;
	std::fwrite ("RIFF", 1, 4, f); std::fwrite (&riff, 4, 1, f); std::fwrite ("WAVEfmt ", 1, 8, f); std::fwrite (&sz, 4, 1, f);
	std::fwrite (&fmt, 2, 1, f); std::fwrite (&ch, 2, 1, f); std::fwrite (&sr, 4, 1, f); std::fwrite (&br, 4, 1, f); std::fwrite (&ba, 2, 1, f); std::fwrite (&bps, 2, 1, f);
	std::fwrite ("data", 1, 4, f); std::fwrite (&dataBytes, 4, 1, f);
	for (uint32_t i = 0; i < n; ++i) { int16_t a = (int16_t) std::lround (clampf (L[i], -1, 1) * 32767), b = (int16_t) std::lround (clampf (R[i], -1, 1) * 32767); std::fwrite (&a, 2, 1, f); std::fwrite (&b, 2, 1, f); }
	std::fclose (f);
}

int main (int argc, char** argv)
{
	const std::string demoDir = argc > 1 ? argv[1] : ".";
	ParamValues base; base.morph = 0; base.carve = 0; base.groove = 0; base.wet = 1; base.output = 0.889f; base.freeze = false;

	std::printf ("1) FFT : aller-retour\n");
	{
		Fft f; f.init (1024); std::vector<float> x (2048), y;
		std::mt19937 g (3); std::uniform_real_distribution<float> d (-1, 1); for (auto& v : x) v = d (g); y = x;
		f.forward (y.data ()); f.inverse (y.data ());
		double err = 0; for (size_t i = 0; i < x.size (); ++i) err = std::max (err, (double) std::fabs (x[i] - y[i]));
		CHECK (err < 1e-5, "erreur max %.2e", err);
	}

	std::printf ("2) Resynthèse STFT transparente (MORPH=0, CARVE=0) et alignement dry/wet\n");
	{
		Engine a, b; a.prepare (SR); b.prepare (SR);
		ParamValues pw = base, pd = base; pd.wet = 0;
		a.setParams (pw); b.setParams (pd);
		std::vector<NoteEvent> notes = { on (100, 48), on (100, 55), on (100, 64) };
		const size_t n = (size_t) (1.5 * SR);
		Run w = render (a, {}, n, notes), d = render (b, {}, n, notes);
		double num = 0, den = 0; for (size_t i = 8000; i < n; ++i) { double e = w.L[i] - d.L[i]; num += e * e; den += d.L[i] * d.L[i]; }
		double db = 10 * std::log10 (num / den + 1e-20);
		CHECK (db < -30.0, "écart wet/dry : %.1f dB (attendu < -30)", db);
		CHECK (a.latencySamples () == 1024, "latence déclarée : %d échantillons (%.1f ms)", a.latencySamples (), 1000.0 * a.latencySamples () / SR);
	}

	std::printf ("3) MORPH : l'enveloppe de la voix source s'imprime sur l'oscillateur\n");
	{
		auto a = vowel (800, 1200, 140, 3.0), ii = vowel (300, 2400, 140, 3.0);
		auto run = [&] (const std::vector<float>& sc, float morph) {
			Engine e; e.prepare (SR); ParamValues p = base; p.morph = morph; e.setParams (p);
			Run r = render (e, sc, sc.size (), { on (0, 45) });
			double A = bandEnergyDb (r.L, (size_t) SR, sc.size (), 600, 1500), B = bandEnergyDb (r.L, (size_t) SR, sc.size (), 2000, 2800);
			return A - B; };
		double ra = run (a, 1), ri = run (ii, 1), r0a = run (a, 0), r0i = run (ii, 0);
		CHECK (ra - ri > 10.0, "MORPH=1 : /a/ vs /i/  -> contraste %.1f dB (attendu > 10)", ra - ri);
		CHECK (std::fabs (r0a - r0i) < 2.0, "MORPH=0 : indépendant de la source (%.1f dB d'écart)", std::fabs (r0a - r0i));
	}

	std::printf ("4) CARVE : creusement des formants dominants du sidechain\n");
	{
		// bruit large + crête étroite de bruit autour de 1 kHz
		auto nz = whiteNoise (3.0, 0.02f, 5); auto pk = whiteNoise (3.0, 0.3f, 9);
		Resonator r1, r2; r1.set (1000, 60); r2.set (1000, 60); for (size_t i = 0; i < pk.size (); ++i) pk[i] = 0.05f * r2.tick (r1.tick (pk[i]));
		std::vector<float> sc (nz.size ()); for (size_t i = 0; i < sc.size (); ++i) sc[i] = nz[i] + pk[i];
		auto run = [&] (float carve) { Engine e; e.prepare (SR); ParamValues p = base; p.carve = carve; e.setParams (p);
			Run r = render (e, sc, sc.size (), { on (0, 45), on (0, 57) }); return bandEnergyDb (r.L, (size_t) SR, sc.size (), 880, 1130); };
		double e0 = run (0), e1 = run (1), eh = run (0.5);
		CHECK (e0 - e1 > 8.0, "CARVE=1 : %.1f dB de creusement dans la bande 0,9-1,1 kHz (attendu > 8, max 24)", e0 - e1);
		CHECK (e0 - eh > 3.0 && eh > e1, "CARVE=0,5 : %.1f dB (monotone)", e0 - eh);
	}

	std::printf ("5) GROOVE : la dynamique du sidechain pilote le gain et le filtre\n");
	{
		const size_t n = (size_t) (4 * SR); std::vector<float> sc (n, 0.0f); auto nz = whiteNoise (4, 1.0f, 2);
		for (size_t i = 0; i < n; ++i) { double t = (double) i / SR; double ph = std::fmod (t, 0.25); sc[i] = nz[i] * 0.5f * (float) std::exp (-ph * 28.0); }
		auto run = [&] (float groove) { Engine e; e.prepare (SR); ParamValues p = base; p.groove = groove; p.wet = 0; e.setParams (p);
			Run r = render (e, sc, n, { on (0, 40), on (0, 47), on (0, 52) });
			// corrélation des enveloppes RMS par trames de 10 ms (sortie décalée de la latence)
			const int F = 480; std::vector<double> a, b;
			for (size_t s = 1024 + 2 * F * 10; s + F + 1024 < n; s += F) { a.push_back (rms (sc, s - 1024, s - 1024 + F)); b.push_back (rms (r.L, s, s + F)); }
			double ma = 0, mb = 0; for (size_t i = 0; i < a.size (); ++i) { ma += a[i]; mb += b[i]; } ma /= a.size (); mb /= b.size ();
			double c = 0, va = 0, vb = 0; for (size_t i = 0; i < a.size (); ++i) { c += (a[i] - ma) * (b[i] - mb); va += (a[i] - ma) * (a[i] - ma); vb += (b[i] - mb) * (b[i] - mb); }
			return c / std::sqrt (va * vb + 1e-20); };
		double c0 = run (0), c1 = run (1);
		CHECK (c1 > 0.6, "GROOVE=1 : corrélation enveloppe sidechain/sortie = %.2f (attendu > 0,6)", c1);
		CHECK (std::fabs (c0) < 0.3, "GROOVE=0 : sortie indépendante du sidechain (corr = %.2f)", c0);
	}

	std::printf ("6) FREEZE : le timbre capturé continue sans le sidechain\n");
	{
		auto a = vowel (800, 1200, 140, 2.0), ii = vowel (300, 2400, 140, 2.5);
		std::vector<float> sc = a; sc.insert (sc.end (), ii.begin (), ii.end ());
		const size_t tFreeze = (size_t) (1.9 * SR);
		auto run = [&] (bool freeze) { Engine e; e.prepare (SR); ParamValues p = base; p.morph = 1; e.setParams (p);
			std::vector<std::pair<size_t, ParamValues>> ch; if (freeze) { p.freeze = true; ch.push_back ({ tFreeze, p }); }
			Run r = render (e, sc, sc.size (), { on (0, 45) }, ch);
			double A = bandEnergyDb (r.L, (size_t) (3.2 * SR), sc.size (), 600, 1500), B = bandEnergyDb (r.L, (size_t) (3.2 * SR), sc.size (), 2000, 2800);
			return A - B; };
		double live = run (false), frozen = run (true);
		CHECK (frozen - live > 8.0, "après bascule en /i/ : live %.1f dB, figé sur /a/ %.1f dB (écart %.1f, attendu > 8)", live, frozen, frozen - live);
		// sidechain coupé après capture : le son continue (table d'ondes mono-cycle)
		Engine e; e.prepare (SR); ParamValues p = base; p.morph = 1; e.setParams (p);
		std::vector<float> sc2 = a; sc2.resize ((size_t) (4 * SR), 0.0f); p.freeze = true;
		Run r = render (e, sc2, sc2.size (), { on (0, 45) }, { { tFreeze, p } });
		double rm = rms (r.L, (size_t) (3 * SR), (size_t) (4 * SR));
		CHECK (rm > 0.005, "sidechain muet, FREEZE actif : RMS = %.4f (le synthé continue de jouer)", rm);
	}

	std::printf ("7) Temps réel : zéro allocation dans process() (freeze inclus)\n");
	{
		Engine e; e.prepare (SR); ParamValues p = base; p.morph = 0.8f; p.carve = 0.5f; p.groove = 0.5f; e.setParams (p);
		auto sc = vowel (700, 1100, 150, 2.0); std::vector<float> L (256), R (256);
		std::vector<NoteEvent> ev = { on (0, 48), on (0, 60) };
		g_allocs = 0; g_count = true;
		for (size_t pos = 0; pos + 256 <= sc.size (); pos += 256)
		{
			if (pos == 48000) { p.freeze = true; e.setParams (p); }
			if (pos == 80000) { p.freeze = false; e.setParams (p); }
			e.process (sc.data () + pos, sc.data () + pos, L.data (), R.data (), 256, pos == 0 ? ev.data () : nullptr, pos == 0 ? 2 : 0);
		}
		g_count = false;
		CHECK (g_allocs == 0, "%ld allocation(s) pendant %.1f s de process()", g_allocs, sc.size () / SR);
	}

	std::printf ("8) Robustesse numérique et charge CPU\n");
	{
		Engine e; e.prepare (SR); ParamValues p = base; p.morph = 1; p.carve = 1; p.groove = 1; p.wet = 0.7f; e.setParams (p);
		auto sc = whiteNoise (10, 0.3f, 4);
		std::vector<NoteEvent> ev; for (int i = 0; i < 16; ++i) ev.push_back (on (0, 36 + 3 * i, 0.9f));
		auto t0 = std::chrono::steady_clock::now ();
		Run r = render (e, sc, sc.size (), ev);
		double secs = std::chrono::duration<double> (std::chrono::steady_clock::now () - t0).count ();
		bool finite = true; double peak = 0; for (size_t i = 0; i < r.L.size (); ++i) { if (!std::isfinite (r.L[i]) || !std::isfinite (r.R[i])) finite = false; peak = std::max (peak, (double) std::fabs (r.L[i])); }
		CHECK (finite, "aucune valeur NaN/inf, crête %.2f", peak);
		CHECK (secs < 5.0, "16 voix + tous les modules : %.2f s de calcul pour 10 s d'audio -> %.1f %% d'un cœur", secs, 100.0 * secs / 10.0);
	}

	// ---- démo écoutable
	{
		std::printf ("Démo : rendu de mimic_demo.wav / sidechain.wav\n");
		const double D = 8.0; const size_t n = (size_t) (D * SR);
		std::vector<float> sc;
		struct V { float f1, f2, len; } syl[] = { { 800, 1200, 0.5f }, { 300, 2400, 0.5f }, { 350, 800, 0.5f }, { 600, 1700, 0.5f }, { 800, 1200, 0.5f }, { 450, 1100, 0.5f }, { 300, 2400, 0.5f }, { 700, 1100, 0.5f } };
		float f0 = 130; for (int rep = 0; rep < 2; ++rep) for (auto& s : syl) { auto v = vowel (s.f1, s.f2, f0, s.len, 0.9f); size_t m = v.size (); for (size_t i = 0; i < m; ++i) { float env = std::min (1.0f, std::min (i / 800.0f, (m - i) / 2400.0f)); v[i] *= env; } sc.insert (sc.end (), v.begin (), v.end ()); f0 = 120 + 25 * std::sin ((float) sc.size () / 40000.0f); }
		sc.resize (n, 0.0f);
		Engine e; e.prepare (SR); ParamValues p = base; p.morph = 1; p.carve = 0.3f; p.groove = 0.35f; p.wet = 1.0f; e.setParams (p);
		std::vector<NoteEvent> ev; int chord[2][3] = { { 45, 52, 57 }, { 41, 48, 53 } };
		for (int i = 0; i < 16; ++i) { int off = (int) (i * 0.5 * SR); for (int k = 0; k < 3; ++k) ev.push_back (on (off, chord[(i / 4) % 2][k] + (i % 4 == 2 ? 12 : 0), 0.8f)); }
		// notes tenues : on relâche au début de chaque mesure
		std::vector<NoteEvent> evAll; for (auto& x : ev) evAll.push_back (x);
		for (int i = 0; i < 16; ++i) { int off = (int) ((i * 0.5 + 0.45) * SR); for (int k = 0; k < 3; ++k) evAll.push_back ({ off, false, chord[(i / 4) % 2][k] + (i % 4 == 2 ? 12 : 0), 0.0f }); }
		std::sort (evAll.begin (), evAll.end (), [] (const NoteEvent& a, const NoteEvent& b) { return a.offset < b.offset; });
		Run r = render (e, sc, n, evAll);
		double pk = 0; for (size_t i = 0; i < n; ++i) pk = std::max (pk, (double) std::max (std::fabs (r.L[i]), std::fabs (r.R[i]))); float g = pk > 0.0 ? (float) (0.89 / pk) : 1.0f;
		for (size_t i = 0; i < n; ++i) { r.L[i] *= g; r.R[i] *= g; }
		writeWav ((demoDir + "/mimic_demo.wav").c_str (), r.L, r.R);
		std::vector<float> scn = sc; for (auto& v : scn) v *= 1.0f; writeWav ((demoDir + "/sidechain.wav").c_str (), scn, scn);
	}

	// ---- Régression : sidechain muet / note rejouée (le niveau ne doit pas s'effondrer)
	{
		std::printf ("Sidechain muet et notes rejouées\n");
		const size_t n = (size_t) (SR * 20);
		std::vector<NoteEvent> ev;
		for (int k = 0; k < 4; ++k) { ev.push_back (on ((int) (k * 5 * SR), 60)); ev.push_back ({ (int) (k * 5 * SR + 0.7 * SR), false, 60, 0.0f }); }
		auto level = [&] (const Run& r, int k) { return rms (r.L, (size_t) (k * 5 * SR + 0.25 * SR), (size_t) (k * 5 * SR + 0.65 * SR)); };
		Engine a; a.prepare (SR); a.setParams (ParamValues {});
		Run none = render (a, {}, n, ev);                       // entrée sidechain absente
		Engine b; b.prepare (SR); b.setParams (ParamValues {});
		Run silent = render (b, std::vector<float> (n, 0.0f), n, ev); // entrée présente mais muette (cas FL Studio)
		double worst = 0; for (int k = 0; k < 4; ++k) worst = std::max (worst, std::fabs (20 * std::log10 (level (silent, k) / level (none, k))));
		CHECK (worst < 0.5, "sidechain muet == sans sidechain (écart max %.2f dB)", worst);
		double lo = 1e9, hi = 0; for (int k = 0; k < 4; ++k) { lo = std::min (lo, (double) level (silent, k)); hi = std::max (hi, (double) level (silent, k)); }
		CHECK (20 * std::log10 (hi / lo) < 3.0, "4 notes rejouées à 5 s d'intervalle : niveaux à %.2f dB près", 20 * std::log10 (hi / lo));
		// Sidechain présent au début puis qui se tait : le niveau revient au neutre au lieu de s'éteindre
		std::vector<float> sc (n, 0.0f); for (size_t i = 0; i < (size_t) (SR * 3); ++i) sc[i] = 0.3f * std::sin (i * 0.03f) * std::sin (i * 0.0005f);
		Engine c; c.prepare (SR); c.setParams (ParamValues {});
		Run mix = render (c, sc, n, ev);
		CHECK (level (mix, 3) > 0.8 * level (none, 3), "après un sidechain qui s'arrête, le synthé retrouve son niveau (%.3f vs %.3f)", level (mix, 3), level (none, 3));
	}

	// ---- Formes d'onde et BLEND
	{
		std::printf ("Formes d'onde et BLEND\n");
		const size_t n = (size_t) (SR * 3);
		auto shape = [&] (int w) { Engine e; e.prepare (SR); ParamValues p; p.morph = 0; p.carve = 0; p.groove = 0; p.wet = 1; p.wave = w; e.setParams (p);
			return render (e, {}, n, { on (0, 45) }); };   // La1 = 110 Hz
		auto harm = [&] (const Run& r, int h) { return bandEnergyDb (r.L, (size_t) SR, n, 110.0f * h - 20, 110.0f * h + 20); };
		Run saw = shape (0), sq = shape (1), tri = shape (2), sine = shape (3);
		CHECK (harm (sq, 2) < harm (sq, 3) - 20.0 && harm (sq, 3) > harm (sq, 5), "carré : harmonique 2 absente, impaires décroissantes");
		CHECK (harm (saw, 2) > harm (saw, 3) - 12.0 && harm (saw, 2) > harm (sq, 2) + 15.0, "scie : harmoniques paires présentes");
		CHECK (harm (sine, 1) - harm (sine, 3) > 40.0, "sinus : pur (H1-H3 = %.1f dB)", harm (sine, 1) - harm (sine, 3));
		CHECK (harm (tri, 1) - harm (tri, 3) > 15.0 && harm (tri, 1) - harm (tri, 3) < 30.0, "triangle : H3 à -19 dB env. (%.1f)", harm (tri, 1) - harm (tri, 3));
		// BLEND = 0 : le son de base seul (sidechain retardé de la latence), BLEND = 1 : inchangé
		auto sc = vowel (700, 1100, 150, 2.0);
		auto runB = [&] (float b) { Engine e; e.prepare (SR); ParamValues p; p.morph = 1; p.wet = 1; p.blend = b; e.setParams (p); return render (e, sc, sc.size (), { on (0, 48) }); };
		Run b0 = runB (0.0f), b1 = runB (1.0f), bh = runB (0.5f);
		double err = 0, ref = 0; for (size_t i = 24000; i + 1024 < sc.size (); ++i) { double d = b0.L[i + 1024] - sc[i]; err += d * d; ref += sc[i] * sc[i]; }
		CHECK (10 * std::log10 (err / ref + 1e-20) < -40.0, "BLEND=0 : sortie == sidechain retardé de 1024 (%.1f dB)", 10 * std::log10 (err / ref + 1e-20));
		const double r1 = rms (b1.L, 48000, 90000), rh = rms (bh.L, 48000, 90000);
		CHECK (r1 > 0.0 && rh > 0.4 * r1, "BLEND=0,5 : mélange des deux (%.3f vs %.3f)", rh, r1);
	}

	std::printf ("\n%s (%d échec(s))\n", g_fail ? "TESTS ÉCHOUÉS" : "TOUS LES TESTS PASSENT", g_fail);
	return g_fail ? 1 : 0;
}
