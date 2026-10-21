#pragma once
#include "SourceHistory.h"
#include <array>

namespace oscilloscope
{
enum class ChannelMode { left, right, leftRight, mid, side, midSide };
class ChannelProjector
{
public:
    static int traceCount(ChannelMode mode) noexcept
    { return mode == ChannelMode::leftRight || mode == ChannelMode::midSide ? 2 : 1; }
    static const char* modeName(ChannelMode mode) noexcept
    {
        switch (mode)
        {
            case ChannelMode::left: return "L";
            case ChannelMode::right: return "R";
            case ChannelMode::leftRight: return "L+R";
            case ChannelMode::mid: return "M";
            case ChannelMode::side: return "S";
            case ChannelMode::midSide: return "M+S";
        }
        return "L+R";
    }
    static const char* traceName(ChannelMode mode, int trace) noexcept
    {
        if (mode == ChannelMode::leftRight) return trace == 0 ? "L" : "R";
        if (mode == ChannelMode::midSide) return trace == 0 ? "M" : "S";
        return modeName(mode);
    }
    static std::array<float, 2> project(StereoSample sample, ChannelMode mode) noexcept
    {
        // Halve before adding so large finite display samples cannot overflow.
        const float mid = sample.left * 0.5f + sample.right * 0.5f;
        const float side = sample.left * 0.5f - sample.right * 0.5f;
        switch (mode)
        {
            case ChannelMode::left: return { sample.left, 0 };
            case ChannelMode::right: return { sample.right, 0 };
            case ChannelMode::leftRight: return { sample.left, sample.right };
            case ChannelMode::mid: return { mid, 0 };
            case ChannelMode::side: return { side, 0 };
            case ChannelMode::midSide: return { mid, side };
        }
        return {};
    }
};
}
