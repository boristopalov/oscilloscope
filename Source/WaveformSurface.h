#pragma once
#include "WaveformRenderer.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace oscilloscope
{
class FramePacer
{
public:
    explicit FramePacer(std::function<void(double)>);
    ~FramePacer();
    bool running() const noexcept;
    void setDisplay(std::uint32_t);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
struct WaveformDraw
{
    WaveformFrame frame;
    juce::Rectangle<float> area; // Logical coordinates relative to the surface.
    juce::Colour colour;
    float range = 2, threshold = 1;
    bool overlay = false;
};
// Only waveform ink is native. JUCE retains controls, grid, labels and snapshots.
// A failed/unavailable device leaves the software renderer usable.
class WaveformSurface final : public juce::NSViewComponent
{
public:
    WaveformSurface();
    ~WaveformSurface() override;
    // Message thread only. Failed resources retry after a bounded cooldown;
    // false keeps the software renderer active in the meantime.
    bool prepare();
    bool available() const noexcept;
    float pixelScale() const noexcept;
    std::uint32_t displayId() const noexcept;
    bool render(const std::vector<WaveformDraw>&, double targetSeconds, double cursor = 0, double preparationMs = 0);
    struct Presentation { double target = 0, presented = 0, cpuMs = 0, gpuMs = 0, cursor = 0, submittedAt = 0, preparationMs = 0; };
    // Optional measurement hook; called on the message thread on later frames.
    std::function<void(Presentation)> onPresented;
    std::uint64_t skippedFrames() const noexcept;
    std::uint64_t failedFrames() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
