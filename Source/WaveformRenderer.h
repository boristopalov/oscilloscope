#pragma once
#include "ChannelProjector.h"
#include "ViewerModel.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace oscilloscope
{
struct WaveformBucket
{
    std::array<float, 2> low {}, high {};
    bool present = false;
};
struct WaveformPoint { float x = 0; std::array<float, 2> values {}; };
struct WaveformFrame
{
    ChannelMode mode = ChannelMode::leftRight;
    std::vector<WaveformBucket> buckets;
    std::vector<WaveformPoint> points;
    std::array<float, 2> peaks {};
    float originalPeak = 0;
    bool individualSamples = false;
    float xOffset = 0, xStep = 1; // Physical-pixel transform for sample-aligned bins.
    float peak() const noexcept { return std::max(peaks[0], peaks[1]); }
};
class WaveformRenderer
{
public:
    // Coordinates are exclusive at end; each integer sample belongs to exactly
    // one pixel bucket. Project samples before accumulating extrema.
    static WaveformFrame analyse(const ViewLane&, double start, double end, int pixels, ChannelMode);
    static WaveformFrame analyseAligned(const ViewLane&, double start, double end, int pixels, ChannelMode);
    static float amplitudeRange(double headroomDb) noexcept;
    static double peakDb(float magnitude) noexcept;
    static juce::String peakText(float magnitude);
    static float yFor(float value, juce::Rectangle<float> area, float range) noexcept;
    static void drawReferences(juce::Graphics&, juce::Rectangle<float>, float range);
    static void drawGrid(juce::Graphics&, juce::Rectangle<float>, const ViewWindow&, bool synced, float range);
    static void rasterize(juce::Image&, const WaveformFrame&, juce::Colour, float range,
                          float threshold = 1, bool overlay = false, float pixelScale = 1);
    static void draw(juce::Graphics&, juce::Rectangle<float>, const WaveformFrame&, juce::Colour, float range,
                     float threshold = 1, float pixelScale = 1);
};
}
