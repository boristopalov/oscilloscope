#pragma once
#include <algorithm>
#include <cmath>

namespace oscilloscope
{
// Message-thread clock in macOS host-time seconds. Correct future velocity,
// never a camera position that has already been issued to a display.
class PresentationClock
{
public:
    static constexpr double delaySeconds = 0.120;
    void clear() noexcept { *this = {}; }
    bool initialized() const noexcept { return ready; }
    double sampleAt(double seconds) const noexcept
    { return sample + (seconds - time) * 48000.0 * rate + delaySeconds * 48000.0; }
    double cursorAt(double seconds) const noexcept
    {
        if (std::abs(seconds - lastPresentedTime) < 1.0e-12) return lastPresentedSample;
        const double value = sampleAt(seconds) - delaySeconds * 48000.0;
        if (seconds > lastPresentedTime) { lastPresentedTime = seconds; lastPresentedSample = value; }
        return value;
    }
    void observe(double observedSample, double capturedAt, double receivedAt = 0) noexcept
    {
        if (! std::isfinite(capturedAt) || capturedAt <= 0 || capturedAt <= lastObservation) return;
        if (! ready)
        {
            sample = observedSample - delaySeconds * 48000.0; time = capturedAt;
            lastObservation = capturedAt; ready = true; return;
        }
        // Packets arrive after capture, sometimes after a camera target has
        // already been submitted. Applying a correction at their old timestamp
        // would retroactively move that camera and cause visible phase jitter.
        const double when = std::max({ capturedAt, receivedAt, lastPresentedTime });
        const double predicted = sampleAt(when);
        const double observedNow = observedSample + (when - capturedAt) * 48000.0;
        rate = 1.0 + std::clamp((observedNow - predicted) / (48000.0 * 2.0), -0.002, 0.002);
        sample = predicted - delaySeconds * 48000.0; time = when; lastObservation = capturedAt;
    }
private:
    double sample = 0, time = 0, rate = 1, lastObservation = 0;
    mutable double lastPresentedTime = 0, lastPresentedSample = 0;
    bool ready = false;
};
}
