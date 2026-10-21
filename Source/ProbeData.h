#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace oscilloscope
{
constexpr double supportedSampleRate = 48000.0;
enum TimingField : std::uint32_t
{
    hasPosition = 1u << 0, hasSamples = 1u << 1, hasPpq = 1u << 2,
    hasBpm = 1u << 3, hasBarStart = 1u << 4, hasMeter = 1u << 5,
    hasSeconds = 1u << 6, hasLoopBounds = 1u << 7, hasHostTime = 1u << 8
};

// Development telemetry only: one small record per callback, no raw audio.
struct ProbeRecord
{
    std::uint64_t generation = 0, sequence = 0;
    std::int64_t samplePosition = 0;
    double ppq = 0, bpm = 0, barStartPpq = 0, seconds = 0;
    double peakL = 0, peakR = 0;
    std::uint32_t valid = 0, nonFinite = 0;
    int blockSize = 0, numerator = 0, denominator = 0;
    int peakOffsetL = -1, peakOffsetR = -1;
    bool playing = false, looping = false, recording = false, doublePrecision = false;
};
static_assert(std::is_trivially_copyable_v<ProbeRecord>);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);

// Exactly one audio producer and one message-thread consumer. Reset never changes indices.
template <typename T, std::size_t Slots>
class SpscQueue
{
public:
    static_assert(Slots > 1);
    bool push(const T& record) noexcept
    {
        const auto write = writeIndex.load(std::memory_order_relaxed);
        const auto next = (write + 1) % Slots;
        if (next == readIndex.load(std::memory_order_acquire))
            return false;
        records[write] = record;
        writeIndex.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& record) noexcept
    {
        const auto read = readIndex.load(std::memory_order_relaxed);
        if (read == writeIndex.load(std::memory_order_acquire))
            return false;
        record = records[read];
        readIndex.store((read + 1) % Slots, std::memory_order_release);
        return true;
    }
private:
    std::array<T, Slots> records {};
    alignas(64) std::atomic<std::uint64_t> writeIndex { 0 };
    alignas(64) std::atomic<std::uint64_t> readIndex { 0 };
};

enum class TimingEvent { initial, steady, unavailable, stopped, looping, captureGap, tempoChanged, discontinuity };
inline const char* eventName(TimingEvent event) noexcept
{
    switch (event)
    {
        case TimingEvent::initial: return "initial";
        case TimingEvent::steady: return "steady";
        case TimingEvent::unavailable: return "sync_unavailable";
        case TimingEvent::stopped: return "stopped";
        case TimingEvent::looping: return "looping_unsupported";
        case TimingEvent::captureGap: return "capture_gap";
        case TimingEvent::tempoChanged: return "tempo_changed";
        case TimingEvent::discontinuity: return "discontinuity";
    }
    return "unknown";
}

struct Observation
{
    ProbeRecord record;
    TimingEvent event = TimingEvent::initial;
    double sampleError = 0, ppqErrorInSamples = 0;
    bool hasSampleError = false, hasPpqError = false;
};

// These are provisional probe thresholds, NOT measured Live tolerances.
// The CSV keeps raw residuals so host measurements can replace them.
class ContinuityProbe
{
public:
    Observation observe(const ProbeRecord& current) noexcept
    {
        Observation result { current };
        constexpr auto required = hasPosition | hasSamples | hasPpq | hasBpm;
        const bool usable = (current.valid & required) == required
                         && std::isfinite(current.ppq) && std::isfinite(current.bpm) && current.bpm > 0;
        const bool comparable = havePrevious && usable
                             && (previous.valid & required) == required
                             && previous.playing && ! previous.looping
                             && previous.generation == current.generation;
        if (comparable)
        {
            result.hasSampleError = result.hasPpqError = true;
            result.sampleError = static_cast<double>(current.samplePosition - previous.samplePosition)
                               - previous.blockSize;
            result.ppqErrorInSamples = (current.ppq - previous.ppq) * 60.0 * supportedSampleRate
                                    / previous.bpm - previous.blockSize;
        }
        if (! usable) result.event = TimingEvent::unavailable;
        else if (current.looping) result.event = TimingEvent::looping;
        else if (! current.playing) result.event = TimingEvent::stopped;
        else if (havePrevious && previous.generation == current.generation
                 && current.sequence != previous.sequence + 1) result.event = TimingEvent::captureGap;
        else if (comparable && std::abs(current.bpm - previous.bpm) > 1.0e-7)
            result.event = TimingEvent::tempoChanged;
        else if (comparable && (std::abs(result.sampleError) > 1.0
                               || std::abs(result.ppqErrorInSamples) > 1.0))
            result.event = TimingEvent::discontinuity;
        else if (comparable) result.event = TimingEvent::steady;
        previous = current;
        havePrevious = usable;
        return result;
    }
private:
    ProbeRecord previous;
    bool havePrevious = false;
};
}
