// MIMIC — Moteur DSP complet (indépendant du SDK VST3, testable hors plugin).
//  Sidechain -> STFT -> enveloppe spectrale (True Envelope) -> cross-synthèse sur les oscillateurs polyphoniques (MORPH)
//            -> creusement des formants dominants (CARVE) -> suiveur dynamique sur gain + passe-bas (GROOVE)
//  FREEZE : capture atomique d'une trame FFT complète -> enveloppe figée + table d'ondes mono-cycle jouable sans sidechain.
// Règles temps réel : tout est alloué dans prepare(); process() ne fait ni allocation, ni verrou, ni E/S.
#pragma once
#include "analyzer.h"
#include "common.h"
#include "fft.h"
#include "follower.h"
#include "voices.h"
#include <vector>

namespace mimic::dsp {

// Valeurs normalisées 0..1 (identiques à celles de l'interface).
struct ParamValues
{
	float morph = 0.62f, carve = 0.38f, groove = 0.50f, wet = 0.75f, output = 0.889f;
	bool freeze = false;
	int source = 0; // 0 voix, 1 percu, 2 texture
	int wave = 0;   // forme d'onde : 0 saw, 1 carré, 2 triangle, 3 sinus
	float blend = 1.0f; // 0 = son de base seul (sidechain / fichier), 1 = synthé MIMIC seul
};

inline float outputDb (float n) { return -48.0f + n * 54.0f; }       // -48 .. +6 dB
inline float carveDb (float n) { return -24.0f * n; }                  // 0 .. -24 dB

struct NoteEvent
{
	int offset = 0;
	bool on = false;
	int note = 60;
	float vel = 1.0f;
};

// Trame envoyée à l'interface (≈60 Hz) : spectres en 64 bandes log, énergies, vumètres.
struct VisFrame
{
	static constexpr int kBands = 64;
	float src[kBands] = {};
	float synth[kBands] = {};
	float energy = 0.0f, lowE = 0.0f, jaw = 0.0f, freeze = 0.0f, clip = 0.0f;
	float inPk[2] = {0, 0}, outPk[2] = {0, 0};
	uint32_t seq = 0;
};

class Engine
{
public:
	void prepare (double sampleRate, int frameSize = 1024)
	{
		sr_ = sampleRate;
		N_ = frameSize;
		H_ = N_ / 4;
		bins_ = N_ / 2 + 1;
		fft_.init (N_);
		analyzer_.prepare (N_, sr_);
		follower_.prepare (sr_);
		voices_.prepare (sr_);
		win_.resize (static_cast<size_t> (N_));
		for (int i = 0; i < N_; ++i)
			win_[static_cast<size_t> (i)] = 0.5f - 0.5f * std::cos (kTwoPi * i / N_);
		synthScale_ = 1.0f / 1.5f; // Σ Hann² (recouvrement 75 %) = 1,5
		for (int c = 0; c < 2; ++c)
		{
			inRing_[c].assign (static_cast<size_t> (N_), 0.0f);
			acc_[c].assign (static_cast<size_t> (N_), 0.0f);
			dry_[c].assign (static_cast<size_t> (N_), 0.0f);
			srcDelay_[c].assign (static_cast<size_t> (N_), 0.0f);
			spec_[c].assign (static_cast<size_t> (2 * N_), 0.0f);
		}
		gain_.assign (static_cast<size_t> (bins_), 1.0f);
		gm_.assign (static_cast<size_t> (bins_), 1.0f);
		bandLo_.resize (VisFrame::kBands + 1);
		const float fMin = 30.0f, fMax = std::min (18000.0f, 0.45f * static_cast<float> (sr_));
		for (int b = 0; b <= VisFrame::kBands; ++b)
		{
			const float f = fMin * std::pow (fMax / fMin, static_cast<float> (b) / VisFrame::kBands);
			bandLo_[static_cast<size_t> (b)] = std::min (bins_ - 1, std::max (1, static_cast<int> (std::lround (f * N_ / sr_))));
		}
		aMv_ = onePole (0.0004f, sr_);
		aWet_ = onePole (0.020f, sr_);
		aLiveUp_ = onePole (0.015f, sr_);
		aLiveDn_ = onePole (0.150f, sr_);
		idleLimit_ = static_cast<int> (0.75 * sr_);
		reset ();
	}

	void reset ()
	{
		analyzer_.reset ();
		follower_.reset ();
		voices_.reset ();
		lp_.reset ();
		for (int c = 0; c < 2; ++c)
		{
			std::fill (inRing_[c].begin (), inRing_[c].end (), 0.0f);
			std::fill (acc_[c].begin (), acc_[c].end (), 0.0f);
			std::fill (dry_[c].begin (), dry_[c].end (), 0.0f);
			std::fill (srcDelay_[c].begin (), srcDelay_[c].end (), 0.0f);
		}
		w_ = r_ = dp_ = hop_ = 0;
		mvS_ = 1.0f; lpTick_ = 0;
		wetS_ = params_.wet;
		blendS_ = params_.blend;
		outS_ = gainFor (params_.output);
		normS_ = 1.0f;
		liveS_ = 0.0f;
		idle_ = idleLimit_; // tant qu'aucun signal n'est arrivé : sidechain considéré absent -> rendu neutre
		frozenNow_ = false;
		vis_ = VisFrame {};
		peakIn_[0] = peakIn_[1] = peakOut_[0] = peakOut_[1] = 0.0f;
		clip_ = 0.0f;
	}

	void setParams (const ParamValues& p)
	{
		params_ = p;
		analyzer_.setSource (static_cast<Source> (std::min (2, std::max (0, p.source))));
		voices_.setShape (std::min (3, std::max (0, p.wave)));
	}

	// Retard introduit (analyse + resynthèse) : à déclarer à l'hôte, le chemin dry est aligné dessus.
	// Source « fichier » : le silence après la fin du son fait partie du jeu (pluck court) — pas de retour au neutre.
	void setForceLive (bool on) { forceLive_ = on; }
	int latencySamples () const { return N_; }
	int frameSize () const { return N_; }
	int activeVoices () const { return voices_.activeVoices (); }
	const VisFrame& visual () const { return vis_; }
	bool frozen () const { return frozenNow_; }
	float grooveValue () const { return mvS_; }

	void noteOn (int note, float vel) { voices_.noteOn (note, vel); }
	void noteOff (int note) { voices_.noteOff (note); }
	void allNotesOff () { voices_.allNotesOff (); }

	// scL/scR : sidechain (peuvent être nullptr). Les événements doivent être triés par offset.
	void process (const float* scL, const float* scR, float* outL, float* outR, int n, const NoteEvent* ev, int nev)
	{
		const bool hasSc = scL != nullptr;
		int ei = 0;
		const float groove = params_.groove, morphMix = params_.morph;
		(void) morphMix;
		const float targetWet = params_.wet, targetOut = gainFor (params_.output);
		const int mask = N_ - 1;
		for (int i = 0; i < n; ++i)
		{
			while (ei < nev && ev[ei].offset <= i)
			{
				if (ev[ei].on && ev[ei].vel > 0.0f)
					voices_.noteOn (ev[ei].note, ev[ei].vel);
				else
					voices_.noteOff (ev[ei].note);
				++ei;
			}
			float sc = 0.0f;
			if (hasSc)
			{
				sc = scR ? 0.5f * (scL[i] + scR[i]) : scL[i];
				peakIn_[0] = std::max (peakIn_[0], std::fabs (scL[i]));
				peakIn_[1] = std::max (peakIn_[1], std::fabs (scR ? scR[i] : scL[i]));
			}
			analyzer_.push (sc);
			// Présence du sidechain : silence numérique prolongé (ou entrée non branchée) => MORPH / CARVE / GROOVE neutres,
			// sinon un sidechain muet « éteindrait » le synthé (GROOVE à fond sur du silence = -6 dB et son sombre).
			const float scAbs = hasSc ? std::max (std::fabs (scL[i]), std::fabs (scR ? scR[i] : scL[i])) : 0.0f;
			if (scAbs > 3e-5f || forceLive_)
				idle_ = 0;
			else if (idle_ < idleLimit_)
				++idle_;
			const float liveT = idle_ >= idleLimit_ ? 0.0f : 1.0f;
			liveS_ += (liveT > liveS_ ? aLiveUp_ : aLiveDn_) * (liveT - liveS_);
			float mv = hasSc ? follower_.process (sc) : 1.0f;
			mv = 1.0f + liveS_ * (mv - 1.0f);
			mvS_ += aMv_ * (mv - mvS_);

			float oL, oR;
			voices_.render (oL, oR);
			// GROOVE : l'amplitude de la piste auxiliaire module le gain des voix et l'ouverture du passe-bas
			const float mvE = std::max (mvS_, 0.03f);
			const float vca = 1.0f - groove * (1.0f - mvE);
			if ((lpTick_++ & 15) == 0)
			{
				const float lo = std::log (250.0f), hi = std::log (16000.0f);
				const float fc = std::exp (lerpf (hi, lerpf (lo, hi, mvE), groove));
				lp_.setCutoff (fc, sr_);
			}
			oL = lp_.process (0, oL * vca);
			oR = lp_.process (1, oR * vca);

			inRing_[0][static_cast<size_t> (w_)] = oL;
			inRing_[1][static_cast<size_t> (w_)] = oR;
			const float dL = dry_[0][static_cast<size_t> (dp_)], dR = dry_[1][static_cast<size_t> (dp_)];
			// son de base (sidechain ou fichier), retardé comme le synthé pour rester aligné
			const float srL = srcDelay_[0][static_cast<size_t> (dp_)], srR = srcDelay_[1][static_cast<size_t> (dp_)];
			srcDelay_[0][static_cast<size_t> (dp_)] = hasSc ? scL[i] : 0.0f;
			srcDelay_[1][static_cast<size_t> (dp_)] = hasSc ? (scR ? scR[i] : scL[i]) : 0.0f;
			dry_[0][static_cast<size_t> (dp_)] = oL;
			dry_[1][static_cast<size_t> (dp_)] = oR;
			const float wL = acc_[0][static_cast<size_t> (r_)], wR = acc_[1][static_cast<size_t> (r_)];
			acc_[0][static_cast<size_t> (r_)] = acc_[1][static_cast<size_t> (r_)] = 0.0f;
			w_ = (w_ + 1) & mask;
			r_ = (r_ + 1) & mask;
			dp_ = (dp_ + 1) & mask;

			wetS_ += aWet_ * (targetWet - wetS_);
			outS_ += aWet_ * (targetOut - outS_);
			blendS_ += aWet_ * (params_.blend - blendS_);
			const float gSyn = blendS_ >= 0.9999f ? 1.0f : std::sin (blendS_ * 1.5707963f);  // équi-puissance : 1 = synthé seul
			const float gSrc = blendS_ >= 0.9999f ? 0.0f : std::cos (blendS_ * 1.5707963f);  // 0 = son de base seul
			float yL = ((dL * (1.0f - wetS_) + wL * wetS_) * gSyn + srL * gSrc) * outS_;
			float yR = ((dR * (1.0f - wetS_) + wR * wetS_) * gSyn + srR * gSrc) * outS_;
			if (!(std::fabs (yL) < 8.0f)) yL = yL > 0 ? 8.0f : (yL < 0 ? -8.0f : 0.0f); // protège aussi contre NaN
			if (!(std::fabs (yR) < 8.0f)) yR = yR > 0 ? 8.0f : (yR < 0 ? -8.0f : 0.0f);
			outL[i] = yL;
			outR[i] = yR;
			peakOut_[0] = std::max (peakOut_[0], std::fabs (yL));
			peakOut_[1] = std::max (peakOut_[1], std::fabs (yR));

			if (++hop_ >= H_)
			{
				hop_ = 0;
				processHop ();
			}
		}
		// vumètres : décroissance par bloc
		const float dec = std::exp (-static_cast<float> (n) / static_cast<float> (0.25 * sr_));
		for (int c = 0; c < 2; ++c)
		{
			vis_.inPk[c] = std::max (peakIn_[c], vis_.inPk[c] * dec);
			vis_.outPk[c] = std::max (peakOut_[c], vis_.outPk[c] * dec);
			peakIn_[c] = peakOut_[c] = 0.0f;
		}
		if (std::max (vis_.outPk[0], vis_.outPk[1]) > 1.0f)
			clip_ = 1.0f;
		else
			clip_ *= dec;
		vis_.clip = clip_;
	}

private:
	static float gainFor (float n) { return dbToGain (outputDb (n)); }

	void processHop ()
	{
		const bool frz = params_.freeze;
		analyzer_.analyseFrame (frz);

		// --- FREEZE : capture atomique + construction étalée de la table d'ondes
		if (frz && !frozenNow_)
		{
			analyzer_.capture ();
			voices_.table ().begin (analyzer_.frozenSpectrum (), analyzer_.frozenLogEnvelope (), N_, sr_);
		}
		if (!frz && frozenNow_)
		{
			voices_.setWavetableMode (false);
			voices_.table ().invalidate ();
		}
		frozenNow_ = frz;
		if (frz)
		{
			if (voices_.table ().building ())
				voices_.table ().step ();
			else if (voices_.table ().ready ())
				voices_.setWavetableMode (true);
		}

		// --- Spectres des deux canaux de synthèse
		const int mask = N_ - 1;
		for (int c = 0; c < 2; ++c)
		{
			float* s = spec_[c].data ();
			for (int i = 0; i < N_; ++i)
			{
				s[2 * i] = inRing_[c][static_cast<size_t> ((w_ + i) & mask)] * win_[static_cast<size_t> (i)];
				s[2 * i + 1] = 0.0f;
			}
			fft_.forward (s);
		}

		// --- Gains : MORPH (imprégnation de l'enveloppe) x CARVE (creusement), énergie du MORPH conservée
		const float* shape = analyzer_.shape ();
		const float* cmask = analyzer_.carveMask ();
		const float depth = 1.25f * params_.morph * liveS_;
		float p0 = 0.0f, p1 = 0.0f;
		for (int k = 0; k < bins_; ++k)
		{
			const float g = std::exp (clampf (depth * shape[k], -7.5f, 0.0f));
			gm_[static_cast<size_t> (k)] = g;
			const float pw = spec_[0][static_cast<size_t> (2 * k)] * spec_[0][static_cast<size_t> (2 * k)] + spec_[0][static_cast<size_t> (2 * k + 1)] * spec_[0][static_cast<size_t> (2 * k + 1)]
			                 + spec_[1][static_cast<size_t> (2 * k)] * spec_[1][static_cast<size_t> (2 * k)] + spec_[1][static_cast<size_t> (2 * k + 1)] * spec_[1][static_cast<size_t> (2 * k + 1)];
			p0 += pw;
			p1 += pw * g * g;
		}
		float norm = p1 > 1e-12f ? std::sqrt (p0 / p1) : 1.0f;
		norm = clampf (norm, 0.25f, 4.0f);
		normS_ += 0.35f * (norm - normS_);
		const float carveK = 2.7631f * params_.carve * liveS_; // 24 dB maxi : ln(10)/20 * 24
		for (int k = 0; k < bins_; ++k)
			gain_[static_cast<size_t> (k)] = gm_[static_cast<size_t> (k)] * normS_ * std::exp (-carveK * cmask[k]);

		// --- Application, IFFT, fenêtre de synthèse, recouvrement-addition
		for (int c = 0; c < 2; ++c)
		{
			float* s = spec_[c].data ();
			for (int k = 0; k < bins_; ++k)
			{
				const float g = gain_[static_cast<size_t> (k)];
				s[2 * k] *= g;
				s[2 * k + 1] *= g;
				if (k > 0 && k < N_ / 2)
				{
					s[2 * (N_ - k)] *= g;
					s[2 * (N_ - k) + 1] *= g;
				}
			}
			if (c == 0)
				updateVisual (s);
			fft_.inverse (s);
			for (int i = 0; i < N_; ++i)
				acc_[c][static_cast<size_t> ((r_ + i) & mask)] += s[2 * i] * win_[static_cast<size_t> (i)] * synthScale_;
		}
	}

	void updateVisual (const float* synthSpec)
	{
		const float* mag = analyzer_.magnitudes ();
		const float ref = static_cast<float> (N_) * 0.25f;
		for (int b = 0; b < VisFrame::kBands; ++b)
		{
			const int a = bandLo_[static_cast<size_t> (b)], e = std::max (a + 1, bandLo_[static_cast<size_t> (b + 1)]);
			float ms = 0.0f, my = 0.0f;
			for (int k = a; k < e && k < bins_; ++k)
			{
				ms = std::max (ms, mag[k]);
				my = std::max (my, std::sqrt (synthSpec[2 * k] * synthSpec[2 * k] + synthSpec[2 * k + 1] * synthSpec[2 * k + 1]));
			}
			vis_.src[b] = clampf ((gainToDb (ms / ref) + 80.0f) / 80.0f, 0.0f, 1.0f);
			vis_.synth[b] = clampf ((gainToDb (my / ref) + 80.0f) / 80.0f, 0.0f, 1.0f);
		}
		vis_.energy = clampf ((analyzer_.levelDb () + 60.0f) / 45.0f, 0.0f, 1.0f);
		float low = 0.0f;
		for (int b = 0; b < 10; ++b)
			low += vis_.src[b];
		vis_.lowE = clampf (low / 10.0f * 1.4f, 0.0f, 1.0f);
		vis_.jaw = vis_.energy;
		vis_.freeze = frozenNow_ ? 1.0f : 0.0f;
		++vis_.seq;
	}

	double sr_ = 48000.0;
	int N_ = 1024, H_ = 256, bins_ = 513, w_ = 0, r_ = 0, dp_ = 0, hop_ = 0;
	ParamValues params_;
	Fft fft_;
	SidechainAnalyzer analyzer_;
	DynamicsFollower follower_;
	StateVarLowpass lp_;
	VoiceBank voices_;
	std::vector<float> win_, inRing_[2], acc_[2], dry_[2], srcDelay_[2], spec_[2], gain_, gm_;
	float blendS_ = 1.0f;
	std::vector<int> bandLo_;
	unsigned lpTick_ = 0;
	float liveS_ = 0.0f, aLiveUp_ = 0.01f, aLiveDn_ = 0.001f;
	int idle_ = 0, idleLimit_ = 36000;
	bool forceLive_ = false;
	float synthScale_ = 0.6667f, aMv_ = 0.05f, aWet_ = 0.001f, mvS_ = 1.0f, wetS_ = 0.75f, outS_ = 1.0f, normS_ = 1.0f;
	float peakIn_[2] = {0, 0}, peakOut_[2] = {0, 0}, clip_ = 0.0f;
	bool frozenNow_ = false;
	VisFrame vis_;
};

} // namespace mimic::dsp
