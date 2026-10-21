#pragma once
#include "SourceRegistry.h"
#include <juce_events/juce_events.h>

namespace oscilloscope
{
enum class HoldReason { none, timingChanged, timingLost, stopped, captureGap,
                        referenceMissing, referenceStale, referenceChanged, unsupportedRate };
class DisplayService final
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void displayUpdated() = 0;
    };
    static DisplayService& instance();
    void openViewer(std::uint64_t ownerId = 0);
    void closeViewer();
    void reset();
    void drain(double nowMs = juce::Time::getMillisecondCounterHiRes());
    bool selectReference(std::uint64_t id); // Explicit choice; Reset still required.
    std::uint64_t referenceId() const noexcept { return reference; }
    std::shared_ptr<SourceState> referenceSource() const;
    bool held() const noexcept { return reason != HoldReason::none; }
    HoldReason holdReason() const noexcept { return reason; }
    juce::String sessionMessage() const;
    bool setOffset(std::uint64_t id, double value, OffsetUnit);
    bool setClipThreshold(std::uint64_t id, double db);
    std::size_t visualizationBytes() const;
    bool reserveSnapshot(std::size_t bytes);
    void releaseSnapshot(std::size_t bytes);
    void addListener(Listener* listener) { listeners.add(listener); }
    void removeListener(Listener* listener) { listeners.remove(listener); }
    bool isCapturing() const noexcept { return (captureState.load(std::memory_order_acquire) & 1u) != 0; }
    // Held sessions keep collecting headers but discard audio; the enable and
    // generation word is read coherently once by the audio callback.
    std::atomic<std::uint64_t> captureState { 0 };
    static constexpr int framesPerSecond = 60;
    static constexpr int packetsPerSourcePerTick = 64;
    static constexpr double staleAfterMs = 500, maxDisplayLagSeconds = 0.25;
    static constexpr std::size_t visualizationBudgetBytes = 256 * 1024 * 1024;
private:
    void resetSession();
    void hold(HoldReason newReason) noexcept { if (! held()) reason = newReason; }
    void notify() { listeners.call([](Listener& listener) { listener.displayUpdated(); }); }
    class ConsumerTimer final : private juce::Timer
    {
    public:
        explicit ConsumerTimer(DisplayService& owner) : service(owner) { startTimerHz(framesPerSecond); }
        ~ConsumerTimer() override { stopTimer(); }
    private:
        void timerCallback() override { service.drain(); }
        DisplayService& service;
    };
    int viewers = 0;
    std::size_t roundRobin = 0, snapshotBytes = 0;
    std::uint64_t reference = 0;
    HoldReason reason = HoldReason::none;
    double sessionStartedMs = 0;
    std::unique_ptr<ConsumerTimer> timer;
    juce::ListenerList<Listener> listeners;
};
}
#if OSCILLOSCOPE_DEV_PROBES
extern "C" __attribute__((visibility("default"))) unsigned int oscilloscopeCaptureSourceCount();
extern "C" __attribute__((visibility("default"))) void oscilloscopeCaptureDrain();
extern "C" __attribute__((visibility("default"))) std::uint64_t oscilloscopeCapturedSamples(unsigned int);
#endif
