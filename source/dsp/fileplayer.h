// MIMIC — lecteur de fichier audio (lecture unique, relancée à chaque note) : fournit au moteur un signal "modulateur" à la place du sidechain hôte.
// Aucune allocation, aucun verrou : le clip est immuable une fois publié.
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace mimic::dsp {

struct AudioClip
{
	static constexpr int kPeaks = 256;
	std::vector<float> l, r;
	double sampleRate = 44100.0;
	std::string name;
	float peaks[kPeaks] = {};

	std::size_t frames () const { return l.size (); }
	double seconds () const { return sampleRate > 0 ? static_cast<double> (l.size ()) / sampleRate : 0.0; }

	void computePeaks ()
	{
		const std::size_t n = l.size ();
		for (int p = 0; p < kPeaks; ++p)
		{
			const std::size_t a = n * static_cast<std::size_t> (p) / kPeaks, b = std::max (a + 1, n * static_cast<std::size_t> (p + 1) / kPeaks);
			float m = 0.0f;
			for (std::size_t i = a; i < b && i < n; ++i)
				m = std::max ({m, std::fabs (l[i]), std::fabs (r[i])});
			peaks[p] = std::min (1.0f, m);
		}
	}
};

class FilePlayer
{
public:
	void setClip (const AudioClip* c) { clip_ = c; pos_ = 0.0; playing_ = false; }
	bool hasClip () const { return clip_ != nullptr && clip_->frames () > 1; }
	// Relance le fichier depuis le début (à chaque nouvelle note) : lecture unique, sans boucle.
	// `ratio` = rapport de hauteur (2^((note-60)/12)) : la hauteur change, la durée reste identique (étirement temporel
	// par grains à recouvrement 75 %, comme le mode « stretch » de FL). ratio == 1 : lecture directe.
	void trigger (double ratio = 1.0)
	{
		pos_ = 0.0;
		playing_ = hasClip ();
		ratio_ = std::min (4.0, std::max (0.25, ratio));
		spawn_ = 0;
		started_ = false;
	}
	bool playing () const { return playing_; }
	double position () const { return pos_; }

	// Écrit n échantillons (lecture en boucle, interpolation linéaire, rééchantillonnage vers la fréquence hôte).
	void fill (float* outL, float* outR, int n, double hostSr)
	{
		if (!hasClip () || !playing_)
		{
			std::fill (outL, outL + n, 0.0f);
			std::fill (outR, outR + n, 0.0f);
			return;
		}
		const double step = clip_->sampleRate / hostSr;
		if (std::fabs (ratio_ - 1.0) > 1e-6)
		{
			fillStretched (outL, outR, n, hostSr, step);
			return;
		}
		const double len = static_cast<double> (clip_->frames ());
		const float* cl = clip_->l.data ();
		const float* cr = clip_->r.data ();
		const std::size_t last = clip_->frames () - 1;
		for (int i = 0; i < n; ++i)
		{
			if (!playing_)
			{
				outL[i] = outR[i] = 0.0f;
				continue;
			}
			const std::size_t i0 = static_cast<std::size_t> (pos_);
			const std::size_t i1 = i0 >= last ? last : i0 + 1;
			const float f = static_cast<float> (pos_ - static_cast<double> (i0));
			outL[i] = cl[i0] + f * (cl[i1] - cl[i0]);
			outR[i] = cr[i0] + f * (cr[i1] - cr[i0]);
			pos_ += step;
			if (pos_ >= len)
				playing_ = false; // fin du fichier : silence jusqu'à la prochaine note
		}
	}

private:
	static constexpr int kGrains = 4;
	struct Grain { bool live = false; int age = 0; double base = 0.0; };

	void readAt (double p, float& l, float& r) const
	{
		const double len = static_cast<double> (clip_->frames ());
		if (p < 0.0 || p >= len - 1.0)
		{
			l = r = 0.0f;
			return;
		}
		const std::size_t i0 = static_cast<std::size_t> (p);
		const float f = static_cast<float> (p - static_cast<double> (i0));
		l = clip_->l[i0] + f * (clip_->l[i0 + 1] - clip_->l[i0]);
		r = clip_->r[i0] + f * (clip_->r[i0 + 1] - clip_->r[i0]);
	}

	// WSOLA : choisit le départ du nouveau grain (autour de la position de lecture) qui prolonge au mieux, en phase,
	// le grain précédent — évite les battements entre grains quand on change la hauteur.
	double alignedBase (const Grain& prev, double step) const
	{
		constexpr int W = 160, stride = 3;
		const double inc = step * ratio_ * stride;
		const double D = 0.012 * clip_->sampleRate;
		float ref[W];
		for (int j = 0; j < W; ++j)
		{
			float l, r;
			readAt (prev.base + (static_cast<double> (prev.age) + j * stride) * step * ratio_, l, r);
			ref[j] = l;
		}
		double best = pos_, bestScore = -1e30;
		for (double d = -D; d <= D; d += 3.0)
		{
			double dot = 0.0, en = 1e-9;
			double p = pos_ + d;
			for (int j = 0; j < W; ++j, p += inc)
			{
				float l, r;
				readAt (p, l, r);
				dot += static_cast<double> (ref[j]) * l;
				en += static_cast<double> (l) * l;
			}
			const double score = dot / std::sqrt (en);
			if (score > bestScore)
			{
				bestScore = score;
				best = pos_ + d;
			}
		}
		return best;
	}

	void fillStretched (float* outL, float* outR, int n, double hostSr, double step)
	{
		const int G = std::max (64, static_cast<int> (0.05 * hostSr)); // grain de 50 ms
		const int hop = G / kGrains;
		const double len = static_cast<double> (clip_->frames ());
		if (!started_)
		{
			// un seul grain, démarré à son pic (fenêtre = 1) et lisant la position 0 : l'attaque est conservée
			for (auto& g : grains_)
				g.live = false;
			grains_[0].live = true;
			grains_[0].age = G / 2;
			grains_[0].base = -static_cast<double> (G / 2) * step * ratio_;
			last_ = 0;
			spawn_ = hop;
			started_ = true;
		}
		for (int i = 0; i < n; ++i)
		{
			if (!playing_)
			{
				outL[i] = outR[i] = 0.0f;
				continue;
			}
			if (--spawn_ <= 0)
			{
				spawn_ = hop;
				for (int j = 0; j < kGrains; ++j)
					if (!grains_[j].live)
					{
						grains_[j].live = true;
						grains_[j].age = 0;
						grains_[j].base = alignedBase (grains_[last_], step);
						last_ = j;
						break;
					}
			}
			float sl = 0.0f, sr = 0.0f, sw = 0.0f;
			for (auto& g : grains_)
			{
				if (!g.live)
					continue;
				const float w = 0.5f - 0.5f * std::cos (6.2831853f * static_cast<float> (g.age) / static_cast<float> (G));
				float l, r;
				readAt (g.base + static_cast<double> (g.age) * step * ratio_, l, r);
				sw += w;
				sl += w * l;
				sr += w * r;
				if (++g.age >= G)
					g.live = false;
			}
			const float norm = 1.0f / std::max (sw, 0.25f);
			outL[i] = sl * norm;
			outR[i] = sr * norm;
			pos_ += step;
			if (pos_ >= len)
				playing_ = false;
		}
	}

	const AudioClip* clip_ = nullptr;
	double pos_ = 0.0;
	bool playing_ = false;
	double ratio_ = 1.0;
	int spawn_ = 0;
	int last_ = 0;
	bool started_ = false;
	Grain grains_[kGrains];
};

} // namespace mimic::dsp
