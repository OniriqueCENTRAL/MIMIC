// MIMIC — GROOVE : suiveur dynamique double (crête ultra rapide + RMS intégré) avec auto-normalisation.
#pragma once
#include "common.h"

namespace mimic::dsp {

class DynamicsFollower
{
public:
	void prepare (double sr)
	{
		sr_ = sr;
		aPkUp_ = onePole (0.0003f, sr);
		aPkDn_ = onePole (0.030f, sr);
		aRms_ = onePole (0.012f, sr);
		aRefDn_ = onePole (1.5f, sr);
		reset ();
	}
	void reset () { pk_ = rms2_ = 0.0f; ref_ = 1e-3f; }

	// Retourne la valeur de modulation normalisée 0..1.
	float process (float x)
	{
		const float a = std::fabs (x);
		pk_ += (a > pk_ ? aPkUp_ : aPkDn_) * (a - pk_);
		rms2_ += aRms_ * (x * x - rms2_);
		const float rms = std::sqrt (rms2_);
		// référence lente : monte instantanément, redescend en ~1,5 s (auto-gain)
		ref_ = std::max (pk_, ref_ + aRefDn_ * (1e-4f - ref_));
		ref_ = std::max (ref_, 1e-4f);
		const float mp = pk_ / ref_, mr = rms / ref_;
		const float mv = 0.55f * mp + 0.45f * clampf (mr * 1.6f, 0.0f, 1.0f);
		return clampf (mv, 0.0f, 1.0f);
	}
	float peak () const { return pk_; }
	float rms () const { return std::sqrt (rms2_); }

private:
	double sr_ = 48000.0;
	float aPkUp_ = 0.1f, aPkDn_ = 0.001f, aRms_ = 0.001f, aRefDn_ = 1e-5f;
	float pk_ = 0.0f, rms2_ = 0.0f, ref_ = 1e-3f;
};

// Passe-bas à variables d'état (TPT), 2 pôles, un état par canal.
class StateVarLowpass
{
public:
	void reset () { ic1_[0] = ic2_[0] = ic1_[1] = ic2_[1] = 0.0f; }
	void setCutoff (float fc, double sr, float q = 0.72f)
	{
		fc = clampf (fc, 20.0f, 0.45f * static_cast<float> (sr));
		const float g = std::tan (kPi * fc / static_cast<float> (sr));
		const float k = 1.0f / q;
		a1_ = 1.0f / (1.0f + g * (g + k));
		a2_ = g * a1_;
		a3_ = g * a2_;
	}
	float process (int ch, float v0)
	{
		const float v3 = v0 - ic2_[ch];
		const float v1 = a1_ * ic1_[ch] + a2_ * v3;
		const float v2 = ic2_[ch] + a2_ * ic1_[ch] + a3_ * v3;
		ic1_[ch] = 2.0f * v1 - ic1_[ch];
		ic2_[ch] = 2.0f * v2 - ic2_[ch];
		return v2;
	}

private:
	float a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
	float ic1_[2] = {0, 0}, ic2_[2] = {0, 0};
};

} // namespace mimic::dsp
