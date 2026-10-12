// MIMIC — FFT radix-2 complexe (tableaux entrelacés re,im). Aucune allocation hors init().
#pragma once
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace mimic::dsp {

class Fft
{
public:
	void init (int n)
	{
		n_ = n;
		cos_.resize (static_cast<size_t> (n / 2));
		sin_.resize (static_cast<size_t> (n / 2));
		rev_.resize (static_cast<size_t> (n));
		int bits = 0;
		while ((1 << bits) < n)
			++bits;
		for (int i = 0; i < n; ++i)
		{
			int r = 0;
			for (int b = 0; b < bits; ++b)
				r |= ((i >> b) & 1) << (bits - 1 - b);
			rev_[static_cast<size_t> (i)] = r;
		}
		for (int k = 0; k < n / 2; ++k)
		{
			const double a = -2.0 * 3.14159265358979323846 * k / n;
			cos_[static_cast<size_t> (k)] = static_cast<float> (std::cos (a));
			sin_[static_cast<size_t> (k)] = static_cast<float> (std::sin (a));
		}
	}
	int size () const { return n_; }

	// x : 2*n floats (re0, im0, re1, im1, ...). Non normalisée.
	void forward (float* x) const { run (x, false); }
	// Inverse normalisée par 1/n.
	void inverse (float* x) const
	{
		run (x, true);
		const float s = 1.0f / static_cast<float> (n_);
		for (int i = 0; i < 2 * n_; ++i)
			x[i] *= s;
	}

private:
	void run (float* x, bool inv) const
	{
		const int n = n_;
		for (int i = 0; i < n; ++i)
		{
			const int j = rev_[static_cast<size_t> (i)];
			if (j > i)
			{
				std::swap (x[2 * i], x[2 * j]);
				std::swap (x[2 * i + 1], x[2 * j + 1]);
			}
		}
		const float sgn = inv ? -1.0f : 1.0f;
		for (int len = 2; len <= n; len <<= 1)
		{
			const int half = len >> 1, step = n / len;
			for (int i = 0; i < n; i += len)
			{
				for (int j = 0; j < half; ++j)
				{
					const float wr = cos_[static_cast<size_t> (j * step)];
					const float wi = sgn * sin_[static_cast<size_t> (j * step)];
					const int a = 2 * (i + j), b = 2 * (i + j + half);
					const float tr = x[b] * wr - x[b + 1] * wi;
					const float ti = x[b] * wi + x[b + 1] * wr;
					x[b] = x[a] - tr;
					x[b + 1] = x[a + 1] - ti;
					x[a] += tr;
					x[a + 1] += ti;
				}
			}
		}
	}

	int n_ = 0;
	std::vector<float> cos_, sin_;
	std::vector<int> rev_;
};

} // namespace mimic::dsp
