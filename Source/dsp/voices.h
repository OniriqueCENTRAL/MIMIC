// MIMIC — Oscillateurs polyphoniques : saw PolyBLEP + sous-octave, ou table d'ondes mono-cycle (FREEZE).
#pragma once
#include "common.h"
#include "fft.h"
#include <array>
#include <vector>

namespace mimic::dsp {

inline float polyBlep (float t, float dt)
{
	if (t < dt)
	{
		t /= dt;
		return t + t - t * t - 1.0f;
	}
	if (t > 1.0f - dt)
	{
		t = (t - 1.0f) / dt;
		return t * t + t + t + 1.0f;
	}
	return 0.0f;
}

// Formes d'onde : 0 saw, 1 carré (PolyBLEP), 2 triangle (carré PolyBLEP intégré), 3 sinus. `tri` = état de l'intégrateur.
inline float oscSample (int shape, float ph, float dt, float& tri)
{
	if (shape == 3)
		return 0.85f * std::sin (6.2831853f * ph);
	if (shape == 0)
		return 2.0f * ph - 1.0f - polyBlep (ph, dt);
	float p2 = ph + 0.5f;
	if (p2 >= 1.0f)
		p2 -= 1.0f;
	const float sq = (ph < 0.5f ? 1.0f : -1.0f) + polyBlep (ph, dt) - polyBlep (p2, dt);
	if (shape == 1)
		return 0.62f * sq;
	tri = 0.9998f * tri + 4.0f * dt * sq; // intégrale du carré = triangle d'amplitude ±1
	return tri;
}
inline float triStart (float ph) { return ph < 0.5f ? -1.0f + 4.0f * ph : 3.0f - 4.0f * ph; }

// Table d'ondes mono-cycle à mips (octaves), construite par IFFT depuis le spectre figé.
// La construction est étalée (un mip par appel de step()) pour ne jamais faire de pic de CPU.
class WavetableSet
{
public:
	static constexpr int kSize = 2048;
	static constexpr int kMips = 11;
	static constexpr float kRefHz = 110.0f; // la note à 110 Hz restitue le spectre capturé à l'identique

	void prepare ()
	{
		fft_.init (kSize);
		for (auto& t : tables_)
			t.assign (kSize + 1, 0.0f);
		spec_.assign (static_cast<size_t> (2 * kSize), 0.0f);
		work_.assign (static_cast<size_t> (2 * kSize), 0.0f);
		for (auto& t : building_)
			t.assign (kSize + 1, 0.0f);
		state_ = State::Idle;
		ready_ = false;
	}

	// Prépare la construction à partir d'un spectre complexe d'analyse (N points) et de son enveloppe log absolue.
	// On y met l'excitation "blanchie" (spectre / enveloppe) : l'enveloppe est ré-imposée ensuite par MORPH.
	void begin (const float* frameSpec, const float* logEnv, int N, double sr)
	{
		std::fill (spec_.begin (), spec_.end (), 0.0f);
		const int harmonics = kSize / 2 - 1;
		float energy = 0.0f;
		for (int h = 1; h <= harmonics; ++h)
		{
			const float bin = static_cast<float> (h) * kRefHz * static_cast<float> (N) / static_cast<float> (sr);
			const int k = static_cast<int> (std::lround (bin));
			if (k >= N / 2)
				break;
			const float env = std::exp (logEnv[k]);
			float re = frameSpec[2 * k] / (env + 1e-6f), im = frameSpec[2 * k + 1] / (env + 1e-6f);
			const float m = std::sqrt (re * re + im * im);
			if (m > 6.0f) { re *= 6.0f / m; im *= 6.0f / m; } // borne les pics isolés
			re /= static_cast<float> (h); im /= static_cast<float> (h); // pente de dent de scie : même tilt que l'oscillateur live
			spec_[static_cast<size_t> (2 * h)] = re;
			spec_[static_cast<size_t> (2 * h + 1)] = im;
			energy += re * re + im * im;
		}
		norm_ = energy > 1e-9f ? 1.0f / std::sqrt (energy * 2.0f) * 1.0f : 1.0f;
		nextMip_ = 0;
		ready_ = false;
		// Capture muette (FREEZE enclenché sur du silence) : pas de table, le synthé garde ses oscillateurs au lieu de se taire.
		state_ = energy > 1e-9f ? State::Building : State::Idle;
	}

	// Un mip par appel. Retourne true quand la table est complète (swap fait).
	bool step ()
	{
		if (state_ != State::Building)
			return false;
		const int maxH = std::max (2, (kSize / 2 - 1) >> nextMip_);
		std::fill (work_.begin (), work_.end (), 0.0f);
		for (int h = 1; h <= maxH; ++h)
		{
			work_[static_cast<size_t> (2 * h)] = spec_[static_cast<size_t> (2 * h)];
			work_[static_cast<size_t> (2 * h + 1)] = spec_[static_cast<size_t> (2 * h + 1)];
			work_[static_cast<size_t> (2 * (kSize - h))] = spec_[static_cast<size_t> (2 * h)];
			work_[static_cast<size_t> (2 * (kSize - h) + 1)] = -spec_[static_cast<size_t> (2 * h + 1)];
		}
		fft_.inverse (work_.data ());
		auto& t = building_[static_cast<size_t> (nextMip_)];
		float peak = 1e-9f;
		for (int i = 0; i < kSize; ++i)
			peak = std::max (peak, std::fabs (work_[static_cast<size_t> (2 * i)]));
		for (int i = 0; i < kSize; ++i)
			t[static_cast<size_t> (i)] = work_[static_cast<size_t> (2 * i)];
		t[kSize] = t[0];
		if (nextMip_ == 0)
			refPeak_ = peak;
		// même niveau RMS que mip 0 (les mips plus pauvres en harmoniques ne changent pas de volume apparent)
		++nextMip_;
		if (nextMip_ >= kMips)
		{
			// normalisation commune : RMS = 0.35 pour le mip 0
			float rms = 0.0f;
			for (int i = 0; i < kSize; ++i)
				rms += building_[0][static_cast<size_t> (i)] * building_[0][static_cast<size_t> (i)];
			rms = std::sqrt (rms / kSize) + 1e-9f;
			const float g = 0.35f / rms;
			for (int m = 0; m < kMips; ++m)
				for (int i = 0; i <= kSize; ++i)
					tables_[static_cast<size_t> (m)][static_cast<size_t> (i)] = building_[static_cast<size_t> (m)][static_cast<size_t> (i)] * g;
			state_ = State::Idle;
			ready_ = true;
			return true;
		}
		return false;
	}

	bool ready () const { return ready_; }
	bool building () const { return state_ == State::Building; }
	void invalidate () { ready_ = false; }

	float read (float phase, float f0, double sr) const
	{
		const float maxHarm = 0.45f * static_cast<float> (sr) / std::max (f0, 20.0f);
		int m = 0;
		float h = static_cast<float> (kSize / 2 - 1);
		while (h > maxHarm && m < kMips - 1) { h *= 0.5f; ++m; }
		const float p = phase * kSize;
		const int i = static_cast<int> (p);
		const float fr = p - static_cast<float> (i);
		const auto& t = tables_[static_cast<size_t> (m)];
		return t[static_cast<size_t> (i)] + fr * (t[static_cast<size_t> (i + 1)] - t[static_cast<size_t> (i)]);
	}

private:
	enum class State { Idle, Building };
	Fft fft_;
	std::array<std::vector<float>, kMips> tables_, building_;
	std::vector<float> spec_, work_;
	State state_ = State::Idle;
	int nextMip_ = 0;
	float norm_ = 1.0f, refPeak_ = 1.0f;
	bool ready_ = false;
};

struct Voice
{
	bool active = false, released = false, sustained = false;
	int note = -1;
	float vel = 0.0f, inc = 0.0f, f0 = 0.0f;
	float phL = 0.0f, phR = 0.0f, phSub = 0.0f;
	float triL = 0.0f, triR = 0.0f, triS = 0.0f;
	float env = 0.0f;
	uint64_t age = 0;

	void start (int n, float v, double sr, uint64_t a)
	{
		active = true; released = false; sustained = false; note = n; vel = v; age = a;
		f0 = 440.0f * std::pow (2.0f, (static_cast<float> (n) - 69.0f) / 12.0f);
		inc = f0 / static_cast<float> (sr);
		phL = hash01 (static_cast<uint32_t> (a * 2654435761u + 1u));
		phR = hash01 (static_cast<uint32_t> (a * 40503u + 7u));
		phSub = 0.0f;
		triL = triStart (phL); triR = triStart (phR); triS = triStart (0.0f);
	}
	void release () { released = true; }
};

class VoiceBank
{
public:
	static constexpr int kMaxVoices = 16;

	void prepare (double sr)
	{
		sr_ = sr;
		attackInc_ = 1.0f / (0.003f * static_cast<float> (sr));
		releaseMul_ = std::exp (-1.0f / (0.16f * static_cast<float> (sr)));
		wt_.prepare ();
		reset ();
	}
	void reset ()
	{
		for (auto& v : voices_)
			v = Voice {};
		counter_ = 0;
		wtMix_ = 0.0f;
		bend_ = 1.0f;
		sustain_ = false;
	}

	void noteOn (int note, float vel)
	{
		Voice* slot = nullptr;
		for (auto& v : voices_)
			if (!v.active) { slot = &v; break; }
		if (!slot)
		{
			// Vol de voix : d'abord une voix relâchée (la plus éteinte), à défaut la plus ancienne.
			for (auto& v : voices_)
				if (v.released && (!slot || v.env < slot->env)) slot = &v;
			if (!slot)
			{
				slot = &voices_[0];
				for (auto& v : voices_)
					if (v.age < slot->age) slot = &v;
			}
		}
		slot->start (note, vel, sr_, ++counter_);
	}
	void noteOff (int note)
	{
		for (auto& v : voices_)
			if (v.active && !v.released && v.note == note)
			{
				if (sustain_) v.sustained = true; // pédale tenue : la note sera relâchée avec elle
				else v.release ();
			}
	}
	void allNotesOff ()
	{
		for (auto& v : voices_)
			if (v.active)
				v.release ();
	}

	// Pédale de sustain (CC 64) : les notes relâchées pendant qu'elle est tenue le sont quand elle remonte.
	void setSustain (bool on)
	{
		if (sustain_ && !on)
			for (auto& v : voices_)
				if (v.active && v.sustained) { v.sustained = false; v.release (); }
		sustain_ = on;
	}
	// Molette de hauteur : rapport de fréquence appliqué à toutes les voix.
	void setBend (float ratio) { bend_ = clampf (ratio, 0.25f, 4.0f); }

	WavetableSet& table () { return wt_; }
	void setShape (int s) { shape_ = s; }
	void setWavetableMode (bool on) { wtTarget_ = on && wt_.ready () ? 1.0f : 0.0f; }

	// Un échantillon stéréo (somme des voix)
	void render (float& outL, float& outR)
	{
		wtMix_ += 0.0015f * (wtTarget_ - wtMix_);
		float l = 0.0f, r = 0.0f;
		for (auto& v : voices_)
		{
			if (!v.active)
				continue;
			if (!v.released)
				v.env = std::min (1.0f, v.env + attackInc_);
			else
			{
				v.env *= releaseMul_;
				if (v.env < 1e-4f) { v.active = false; continue; }
			}
			const float dt = std::min (0.49f, v.inc * bend_);
			float sL, sR;
			const float incR = dt * 1.0035f;
			if (wtMix_ < 0.999f)
			{
				const float a = oscSample (shape_, v.phL, dt, v.triL);
				const float b = oscSample (shape_, v.phR, incR, v.triR);
				const float sub = oscSample (shape_, v.phSub, dt * 0.5f, v.triS);
				sL = a + 0.45f * sub;
				sR = b + 0.45f * sub;
			}
			else
				sL = sR = 0.0f;
			if (wtMix_ > 0.001f)
			{
				const float wl = wt_.read (v.phL, v.f0 * bend_, sr_), wr = wt_.read (v.phR, v.f0 * bend_ * 1.0035f, sr_);
				sL = lerpf (sL, wl * 1.6f, wtMix_);
				sR = lerpf (sR, wr * 1.6f, wtMix_);
			}
			const float g = v.env * (0.12f + 0.88f * v.vel) * 0.22f;
			l += sL * g;
			r += sR * g;
			v.phL += dt; if (v.phL >= 1.0f) v.phL -= 1.0f;
			v.phR += incR; if (v.phR >= 1.0f) v.phR -= 1.0f;
			v.phSub += dt * 0.5f; if (v.phSub >= 1.0f) v.phSub -= 1.0f;
		}
		outL = l;
		outR = r;
	}

	int activeVoices () const
	{
		int n = 0;
		for (const auto& v : voices_)
			n += v.active ? 1 : 0;
		return n;
	}

private:
	double sr_ = 48000.0;
	float attackInc_ = 0.01f, releaseMul_ = 0.999f, wtMix_ = 0.0f, wtTarget_ = 0.0f, bend_ = 1.0f;
	bool sustain_ = false;
	int shape_ = 0;
	uint64_t counter_ = 0;
	std::array<Voice, kMaxVoices> voices_ {};
	WavetableSet wt_;
};

} // namespace mimic::dsp
