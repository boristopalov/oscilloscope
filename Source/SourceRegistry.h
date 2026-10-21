#pragma once
#include "CaptureQueue.h"
#include "SourceHistory.h"
#include "TimelineMapper.h"
#include "PresentationClock.h"
#include <memory>
#include <mutex>
#include <vector>

namespace oscilloscope
{
enum class CaptureStatus { waiting, live, syncUnavailable, captureGap, unsupportedRate, stale, incompatible };
const char* captureStatusText(CaptureStatus) noexcept;
enum class OffsetUnit { samples, milliseconds };
struct CaptureSummary
{
    CaptureStatus status = CaptureStatus::waiting;
    std::uint64_t samples = 0, packets = 0, nonFinite = 0;
    CapturePacket latest;
    float peakL = 0, peakR = 0;
};
struct SourceState
{
    explicit SourceState(std::uint64_t runtimeId) : id(runtimeId), colour(palette[(runtimeId - 1) % 8]) {}
    inline static constexpr std::uint32_t palette[] {
        0xff74b2d1, 0xffd2a568, 0xff9bc594, 0xffb79bcf,
        0xffdb94aa, 0xff7fc4b6, 0xffc8c17e, 0xff9eafc6 };
    const std::uint64_t id;
    CaptureQueue queue { CaptureQueue::Storage::deferred };
    std::atomic<double> sampleRate { 0 };
    std::atomic<bool> admitted { false };
    std::atomic<std::uint64_t> droppedCallbacks { 0 }, lostGeneration { 0 };
    std::atomic<std::uint64_t> unsupportedRateRevision { 0 };
    // Solely message-thread data, shared by every viewer.
    CaptureSummary summary;
    SourceHistory history;
    TimelineMapper timeline;
    PresentationClock presentation;
    CaptureHeader observedTransport;
    bool haveTransport = false;
    bool seenAdmitted = false;
    double lastPacketMs = 0;
    std::uint64_t rateRevisionAtReset = 0;
    double offsetValue = 0;
    double clipThresholdDb = 0;
    juce::String nameOverride, hostName;
    std::uint32_t colour;
    bool colourOverridden = false;
    juce::String displayName() const
    { return nameOverride.isNotEmpty() ? nameOverride : hostName.isNotEmpty() ? hostName : "Tap " + juce::String(id); }
    OffsetUnit offsetUnit = OffsetUnit::samples;
    double offsetSamples() const noexcept { return offsetUnit == OffsetUnit::samples ? offsetValue : offsetValue * 48.0; }
};

class SourceRegistry final
{
public:
    static SourceRegistry& instance();
    std::shared_ptr<SourceState> registerSource();
    void unregisterSource(const std::shared_ptr<SourceState>&);
    std::vector<std::shared_ptr<SourceState>> sources() const;
    const juce::String& registryToken() const noexcept { return token; }
    static constexpr std::size_t sourceLimit = 8;
private:
    void updateAdmission(); // Registry mutex held; never called by audio.
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<SourceState>> registered;
    std::uint64_t nextId = 1;
    const juce::String token = juce::Uuid().toString();
};
}
