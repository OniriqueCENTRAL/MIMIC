// MIMIC — utilitaires DSP communs.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mimic::dsp {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

inline float clampf (float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf (float a, float b, float t) { return a + (b - a) * t; }
inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) { return 20.0f * std::log10 (g + 1e-12f); }
inline float smoothstep (float a, float b, float x)
{
	const float t = clampf ((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}
// Coefficient one-pole pour une constante de temps (secondes).
inline float onePole (float seconds, double sr)
{
	return seconds <= 0.0f ? 1.0f : 1.0f - std::exp (-1.0f / (seconds * static_cast<float> (sr)));
}
// Hash déterministe -> [0,1)
inline float hash01 (uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
	return static_cast<float> (x & 0xFFFFFF) / 16777216.0f;
}

} // namespace mimic::dsp
