#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>

namespace naval {
    // Seedable 2D Perlin noise with fractal (fBm) summation. Pure math, no
    // dependencies: being a continuous function samplable at any coordinate is
    // exactly what lets the terrain generate endlessly from a single seed.
    class Noise {
    public:
        explicit Noise(uint32_t seed) {
            std::array<int, 256> base{};
            std::iota(base.begin(), base.end(), 0);
            std::mt19937 rng(seed);
            std::shuffle(base.begin(), base.end(), rng);
            for (int i = 0; i < 256; ++i) {
                m_perm[i] = base[i];
                m_perm[i + 256] = base[i];
            }
        }

        // Classic improved-Perlin noise, output roughly in [-1, 1].
        float Perlin(float x, float y) const {
            int const xi = static_cast<int>(std::floor(x)) & 255;
            int const yi = static_cast<int>(std::floor(y)) & 255;
            float const xf = x - std::floor(x);
            float const yf = y - std::floor(y);
            float const u = Fade(xf);
            float const v = Fade(yf);

            int const aa = m_perm[m_perm[xi] + yi];
            int const ab = m_perm[m_perm[xi] + yi + 1];
            int const ba = m_perm[m_perm[xi + 1] + yi];
            int const bb = m_perm[m_perm[xi + 1] + yi + 1];

            float const x1 = Lerp(Grad(aa, xf, yf), Grad(ba, xf - 1.0f, yf), u);
            float const x2 = Lerp(Grad(ab, xf, yf - 1.0f), Grad(bb, xf - 1.0f, yf - 1.0f), u);
            return Lerp(x1, x2, v);
        }

        // Fractal Brownian motion: octaves of Perlin at rising frequency and
        // falling amplitude, giving large landmasses with finer coastal detail.
        float Fbm(float x, float y, int octaves, float lacunarity, float gain) const {
            float sum = 0.0f;
            float amp = 0.5f;
            float freq = 1.0f;
            for (int i = 0; i < octaves; ++i) {
                sum += amp * Perlin(x * freq, y * freq);
                freq *= lacunarity;
                amp *= gain;
            }
            return sum;
        }

    private:
        static float Fade(float t) { return t * t * t * ((t * ((t * 6.0f) - 15.0f)) + 10.0f); }
        static float Lerp(float a, float b, float t) { return a + (t * (b - a)); }
        static float Grad(int hash, float x, float y) {
            // Pick one of four diagonal gradient directions from the low bits.
            switch (hash & 3) {
            case 0: return x + y;
            case 1: return -x + y;
            case 2: return x - y;
            default: return -x - y;
            }
        }

        std::array<int, 512> m_perm{};
    };
}
