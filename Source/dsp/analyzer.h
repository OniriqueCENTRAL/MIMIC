// MIMIC — Analyse du sidechain : STFT fenêtrée, enveloppe spectrale par True Envelope cepstrale,
// masque CARVE (formants dominants). Toute la mémoire est allouée dans prepare().
#pragma once
#include "common.h"
#include "fft.h"
#include <vector>

namespace mimic::dsp {

enum class Source : int { Voice = 0, Percussion = 1, Texture = 2 };

class SidechainAnalyzer
{
public:
	static constexpr int kTrueEnvelopeIterations = 3;

	void prepare (int frameSize, double sr)
	{
		N_ = frameSize;
		H_ = N_ / 2 + 1;
		sr_ = sr;
		fft_.init (N_);
		ring_.assign (static_cast<size_t> (N_), 0.0f);
		mask_ = N_ - 1;
		win_.resize (static_cast<size_t> (N_));
		for (int i = 0; i < N_; ++i)
			win_[static_cast<size_t> (i)] = 0.5f - 0.5f * std::cos (kTwoPi * i / N_);
		winPow_ = 0.0f;
		for (float w : win_)
			winPow_ += w * w;
		buf_.assign (static_cast<size_t> (2 * N_), 0.0f);
		spec_.assign (static_cast<size_t> (2 * N_), 0.0f);
		frozenSpec_.assign (static_cast<size_t> (2 * N_), 0.0f);
		mag_.assign (static_cast<size_t> (H_), 0.0f);
		lmag_.assign (static_cast<size_t> (H_), 0.0f);
		target_.assign (static_cast<size_t> (H_), 0.0f);
		ls_.assign (static_cast<size_t> (H_), 0.0f);
		lsSm_.assign (static_cast<size_t> (H_), 0.0f);
		frozenLs_.assign (static_cast<size_t> (H_), 0.0f);
		shape_.assign (static_cast<size_t> (H_), 0.0f);
		carveMask_.assign (static_cast<size_t> (H_), 0.0f);
		prefix_.assign (static_cast<size_t> (H_ + 1), 0.0f);
		setSource (Source::Voice);
		reset ();
	}

	void reset ()
	{
		std::fill (ring_.begin (), ring_.end (), 0.0f);
		std::fill (lsSm_.begin (), lsSm_.end (), 0.0f);
		std::fill (shape_.begin (), shape_.end (), 0.0f);
		std::fill (carveMask_.begin (), carveMask_.end (), 0.0f);
		std::fill (mag_.begin (), mag_.end (), 0.0f);
		wpos_ = 0;
		first_ = true;
		levelDb_ = -120.0f;
		hasFrozen_ = false;
	}

	// Voix : formants fins ; Percussion : enveloppe large et réactive ; Texture : détail spectral fin et lent.
	void setSource (Source s)
	{
		source_ = s;
		const float k = static_cast<float> (sr_ / 48000.0);
		switch (s)
		{
			case Source::Voice: lifter_ = static_cast<int> (std::lround (38 * k)); alpha_ = 0.45f; break;
			case Source::Percussion: lifter_ = static_cast<int> (std::lround (14 * k)); alpha_ = 0.85f; break;
			case Source::Texture: lifter_ = static_cast<int> (std::lround (90 * k)); alpha_ = 0.25f; break;
		}
		lifter_ = std::max (4, std::min (lifter_, N_ / 4));
	}

	void push (float x)
	{
		ring_[static_cast<size_t> (wpos_)] = x;
		wpos_ = (wpos_ + 1) & mask_;
	}

	// Analyse les N derniers échantillons. Si frozen : on garde l'enveloppe et le masque tels quels.
	void analyseFrame (bool frozen)
	{
		float acc = 0.0f;
		for (int i = 0; i < N_; ++i)
		{
			const float v = ring_[static_cast<size_t> ((wpos_ + i) & mask_)] * win_[static_cast<size_t> (i)];
			buf_[static_cast<size_t> (2 * i)] = v;
			buf_[static_cast<size_t> (2 * i + 1)] = 0.0f;
			acc += v * v;
		}
		fft_.forward (buf_.data ());
		spec_ = buf_;
		for (int k = 0; k < H_; ++k)
		{
			const float re = buf_[static_cast<size_t> (2 * k)], im = buf_[static_cast<size_t> (2 * k + 1)];
			mag_[static_cast<size_t> (k)] = std::sqrt (re * re + im * im);
			lmag_[static_cast<size_t> (k)] = std::log (mag_[static_cast<size_t> (k)] + 1e-5f);
		}
		const float rms = std::sqrt (acc / winPow_);
		levelDb_ = gainToDb (rms);
		if (frozen)
			return;
		if (levelDb_ < kGateDb)
		{
			// sidechain muet : l'enveloppe est conservée, le masque de creusement retombe
			for (int k = 0; k < H_; ++k)
				carveMask_[static_cast<size_t> (k)] *= 0.8f;
			return;
		}
		trueEnvelope ();
		for (int k = 0; k < H_; ++k)
		{
			float& s = lsSm_[static_cast<size_t> (k)];
			s = first_ ? ls_[static_cast<size_t> (k)] : s + alpha_ * (ls_[static_cast<size_t> (k)] - s);
		}
		first_ = false;
		// forme relative : crête à 0, plancher à -6 népers (≈ -52 dB) pour ne pas écraser la dynamique utile
		float peak = -1e9f;
		for (int k = 1; k < H_ - 1; ++k)
			peak = std::max (peak, lsSm_[static_cast<size_t> (k)]);
		const float floorV = peak - kShapeRangeNepers;
		for (int k = 0; k < H_; ++k)
			shape_[static_cast<size_t> (k)] = std::max (lsSm_[static_cast<size_t> (k)], floorV) - peak;
		updateCarveMask ();
	}

	// Capture atomique pour FREEZE : spectre complexe + enveloppe absolue.
	void capture ()
	{
		frozenSpec_ = spec_;
		frozenLs_ = lsSm_;
		hasFrozen_ = true;
	}

	int frameSize () const { return N_; }
	int numBins () const { return H_; }
	const float* shape () const { return shape_.data (); }          // log-enveloppe relative à sa crête, ≤ 0 (népers)
	const float* carveMask () const { return carveMask_.data (); }  // 0..1
	const float* magnitudes () const { return mag_.data (); }
	float levelDb () const { return levelDb_; }
	bool hasFrozen () const { return hasFrozen_; }
	const float* frozenSpectrum () const { return frozenSpec_.data (); } // entrelacé, N complexes
	const float* frozenLogEnvelope () const { return frozenLs_.data (); }

	static constexpr float kGateDb = -72.0f;
	static constexpr float kShapeRangeNepers = 6.0f;

private:
	// Lissage cepstral itératif : l'enveloppe épouse les crêtes (formants) plutôt que la moyenne.
	void trueEnvelope ()
	{
		for (int k = 0; k < H_; ++k)
			target_[static_cast<size_t> (k)] = lmag_[static_cast<size_t> (k)];
		for (int it = 0; it < kTrueEnvelopeIterations; ++it)
		{
			for (int k = 0; k < H_; ++k)
			{
				buf_[static_cast<size_t> (2 * k)] = target_[static_cast<size_t> (k)];
				buf_[static_cast<size_t> (2 * k + 1)] = 0.0f;
			}
			for (int k = 1; k < H_ - 1; ++k)
			{
				buf_[static_cast<size_t> (2 * (N_ - k))] = target_[static_cast<size_t> (k)];
				buf_[static_cast<size_t> (2 * (N_ - k) + 1)] = 0.0f;
			}
			fft_.inverse (buf_.data ()); // cepstre réel
			for (int n = 0; n < N_; ++n)
			{
				const int d = n <= N_ / 2 ? n : N_ - n;
				float w = 0.0f;
				if (d < lifter_)
					w = d < lifter_ * 3 / 4 ? 1.0f : 0.5f + 0.5f * std::cos (kPi * (d - lifter_ * 0.75f) / (lifter_ * 0.25f));
				buf_[static_cast<size_t> (2 * n)] *= w;
				buf_[static_cast<size_t> (2 * n + 1)] = 0.0f;
			}
			fft_.forward (buf_.data ());
			for (int k = 0; k < H_; ++k)
			{
				ls_[static_cast<size_t> (k)] = buf_[static_cast<size_t> (2 * k)];
				target_[static_cast<size_t> (k)] = std::max (lmag_[static_cast<size_t> (k)], ls_[static_cast<size_t> (k)]);
			}
		}
	}

	// Masque 0..1 des formants qui dépassent nettement la tendance large de l'enveloppe.
	void updateCarveMask ()
	{
		prefix_[0] = 0.0f;
		for (int k = 0; k < H_; ++k)
			prefix_[static_cast<size_t> (k + 1)] = prefix_[static_cast<size_t> (k)] + lsSm_[static_cast<size_t> (k)];
		const int W = std::max (8, N_ / 40);
		const float gate = smoothstep (kGateDb, kGateDb + 30.0f, levelDb_);
		for (int k = 0; k < H_; ++k)
		{
			const int a = std::max (0, k - W), b = std::min (H_ - 1, k + W);
			const float broad = (prefix_[static_cast<size_t> (b + 1)] - prefix_[static_cast<size_t> (a)]) / static_cast<float> (b - a + 1);
			const float dDb = (lsSm_[static_cast<size_t> (k)] - broad) * 8.686f;
			const float tgt = smoothstep (2.0f, 12.0f, dDb) * gate;
			float& m = carveMask_[static_cast<size_t> (k)];
			m += (tgt > m ? 0.6f : 0.15f) * (tgt - m);
		}
	}

	int N_ = 1024, H_ = 513, mask_ = 1023, wpos_ = 0, lifter_ = 38;
	double sr_ = 48000.0;
	float alpha_ = 0.45f, winPow_ = 1.0f, levelDb_ = -120.0f;
	bool first_ = true, hasFrozen_ = false;
	Source source_ = Source::Voice;
	Fft fft_;
	std::vector<float> ring_, win_, buf_, spec_, frozenSpec_, mag_, lmag_, target_, ls_, lsSm_, frozenLs_, shape_, carveMask_, prefix_;
};

} // namespace mimic::dsp
