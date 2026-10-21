#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace oscilloscope
{
struct StereoSample { float left = 0, right = 0; };
// Extrema of actual L, R, M and S samples, never transformed L/R extrema.
struct SampleExtrema
{
    std::array<float, 4> low {}, high {};
    bool present = false;
    void add(StereoSample sample) noexcept
    {
        if (! std::isfinite(sample.left)) sample.left = 0;
        if (! std::isfinite(sample.right)) sample.right = 0;
        const std::array<float, 4> values { sample.left, sample.right,
            sample.left * 0.5f + sample.right * 0.5f, sample.left * 0.5f - sample.right * 0.5f };
        if (! present) { low = high = values; present = true; return; }
        for (std::size_t i = 0; i < 4; ++i)
        { low[i] = std::min(low[i], values[i]); high[i] = std::max(high[i], values[i]); }
    }
    void merge(const SampleExtrema& other) noexcept
    {
        if (! other.present) return;
        if (! present) { *this = other; return; }
        for (std::size_t i = 0; i < 4; ++i)
        { low[i] = std::min(low[i], other.low[i]); high[i] = std::max(high[i], other.high[i]); }
    }
};
}
