#pragma once
#include <cmath>
#include <cstdint>
#include "Define.h"

namespace Market
{
    // The market step's one random source: xoshiro256** seeded by splitmix64. A market
    // step draws tens of thousands of numbers; this keeps each draw a few shifts and
    // multiplies, with no distribution objects built per draw. Seeded once from
    // Random.h's rand32() by the service, or with a fixed seed by the self-tests.
    class Rng
    {
    public:
        explicit Rng(uint64 seed) { Seed(seed); }

        void Seed(uint64 seed)
        {
            for (uint64& word : _s)
            {
                seed += 0x9E3779B97F4A7C15ULL;
                uint64 z = seed;
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
                word = z ^ (z >> 31);
            }
        }

        uint64 Next()
        {
            uint64 const result = Rotl(_s[1] * 5, 7) * 9;
            uint64 const t = _s[1] << 17;
            _s[2] ^= _s[0];
            _s[3] ^= _s[1];
            _s[1] ^= _s[2];
            _s[0] ^= _s[3];
            _s[2] ^= t;
            _s[3] = Rotl(_s[3], 45);
            return result;
        }

        // [0, 1), 53 random bits.
        double Uniform() { return static_cast<double>(Next() >> 11) * 0x1.0p-53; }

        // Standard normal, Box-Muller (one of the pair is discarded; only used for
        // large Poisson means, which are rare).
        double Normal()
        {
            double u1 = 1.0 - Uniform();  // (0, 1]
            double u2 = Uniform();
            return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
        }

        // Poisson(lambda) with exp(-lambda) supplied by the caller (cached per basket
        // row), so the common lambda << 1 case costs one uniform and one compare. Knuth's
        // product method up to lambda 30, a rounded normal above.
        uint32 Poisson(double lambda, double expNegLambda)
        {
            if (lambda <= 0.0)
            {
                return 0;
            }
            if (lambda > 30.0)
            {
                double draw = std::floor(lambda + std::sqrt(lambda) * Normal() + 0.5);
                return draw <= 0.0 ? 0 : static_cast<uint32>(draw);
            }
            uint32 k = 0;
            double p = Uniform();
            while (p > expNegLambda)
            {
                ++k;
                p *= Uniform();
            }
            return k;
        }

        uint32 Poisson(double lambda) { return Poisson(lambda, std::exp(-lambda)); }

    private:
        static uint64 Rotl(uint64 x, int k) { return (x << k) | (x >> (64 - k)); }

        uint64 _s[4] = {};
    };
}
