#pragma once
#include "CaptureQueue.h"

namespace oscilloscope
{
enum class TimingResult { synced, unavailable, stopped, changed, lost };
struct TimingAnchor
{
    std::int64_t sample = 0;
    double ppq = 0, bpm = 120, barOrigin = 0, beatsPerBar = 4;
    bool synced = false, assumedBarOrigin = true, assumedMeter = true;
    double beatAt(double samplePosition, double offsetSamples = 0) const noexcept
    { return ppq + (samplePosition - static_cast<double>(sample) + offsetSamples) * bpm / (60.0 * supportedSampleRate); }
    double sampleAt(double beat, double offsetSamples = 0) const noexcept
    { return static_cast<double>(sample) + (beat - ppq) * 60.0 * supportedSampleRate / bpm - offsetSamples; }
};

// One anchor and one continuity header, irrespective of callback count.
class TimelineMapper
{
public:
    // Live loop-boundary probes measured sub-sample residuals. Retain a
    // one-sample margin for host rounding while rejecting seeks and tempo changes.
    static constexpr double sampleTolerance = 1.0, tempoTolerance = 1.0e-7;
    void clear() noexcept { *this = {}; }
    TimingResult inspect(const CaptureHeader& header) const noexcept
    {
        const bool usable = hasSync(header);
        if (anchor.synced)
        {
            if (! usable) return TimingResult::lost;
            if (std::abs(header.bpm - anchor.bpm) > tempoTolerance) return TimingResult::changed;
            if ((header.valid & hasMeter) != 0 && header.numerator > 0 && header.denominator > 0
                && std::abs(4.0 * header.numerator / header.denominator - anchor.beatsPerBar) > 1.0e-9)
                return TimingResult::changed;
            // Stopped transport clocks need not advance. Resume begins a new
            // host-clock segment in the same continuous display history.
            if (! header.playing || paused()) return TimingResult::synced;
            const double scale = 60.0 * supportedSampleRate / anchor.bpm;
            const double expected = previous.ppq + previous.sampleCount / scale;
            const double wrapBeats = wrapAmount(header);
            // Accept a host sample-clock origin change only at a verified cycle
            // boundary. Live can change an instance's initial origin on its first wrap.
            const bool wrappedMatch = wrapBeats > 0;
            const bool forwardMatch = std::abs((header.ppq - expected) * scale) <= sampleTolerance;
            if (! wrappedMatch && ! forwardMatch) return TimingResult::changed;
            if (! wrappedMatch && std::abs(static_cast<double>(header.samplePosition)
                - static_cast<double>(previous.samplePosition) - previous.sampleCount) > sampleTolerance)
                return TimingResult::changed;
            if (! wrappedMatch && std::abs(static_cast<double>(header.samplePosition) + sampleBias
                - static_cast<double>(nextSample)) > sampleTolerance) return TimingResult::changed;
            const double beat = header.ppq + beatBias + wrapBeats;
            if (! wrappedMatch && std::abs((beat - anchor.beatAt(static_cast<double>(nextSample))) * scale) > sampleTolerance)
                return TimingResult::changed;
            // Fixed meter only. A change is guarded rather than reconstructed.
            if ((header.valid & hasMeter) != 0 && header.numerator > 0 && header.denominator > 0
                && std::abs(4.0 * header.numerator / header.denominator - anchor.beatsPerBar) > 1.0e-9)
                return TimingResult::changed;
            return TimingResult::synced;
        }
        // Initial insufficient timing remains a per-source time fallback until Reset.
        return ! havePrevious && usable ? TimingResult::synced : TimingResult::unavailable;
    }
    std::int64_t accept(const CaptureHeader& header, TimingResult result,
                        double initialBeat = std::numeric_limits<double>::quiet_NaN()) noexcept
    {
        if (! havePrevious)
        {
            anchor.synced = result == TimingResult::synced;
            if (anchor.synced)
            {
                anchor.sample = header.samplePosition;
                anchor.ppq = std::isfinite(initialBeat) ? initialBeat : header.ppq;
                beatBias = anchor.ppq - header.ppq; anchor.bpm = header.bpm;
                anchor.assumedBarOrigin = (header.valid & hasBarStart) == 0 || ! std::isfinite(header.barStartPpq);
                anchor.barOrigin = anchor.assumedBarOrigin ? 0 : header.barStartPpq + beatBias;
                anchor.assumedMeter = (header.valid & hasMeter) == 0 || header.numerator <= 0 || header.denominator <= 0;
                anchor.beatsPerBar = anchor.assumedMeter ? 4 : 4.0 * header.numerator / header.denominator;
                nextSample = anchor.sample;
            }
        }
        const auto previousEnd = nextSample;
        const bool resuming = havePrevious && paused() && header.playing;
        if (havePrevious && (paused() || ! header.playing) && previous.captureTime > 0 && header.captureTime > 0)
        {
            const double gap = header.captureTime - previous.captureTime - previous.sampleCount / supportedSampleRate;
            // Ignore normal callback jitter. A suspended callback stream leaves
            // an explicit unavailable interval, never fabricated zero samples.
            if (gap > 0.1)
                nextSample += static_cast<std::int64_t>(std::llround(gap * supportedSampleRate));
        }
        if (resuming && anchor.synced && std::isfinite(initialBeat))
            nextSample = std::max(previousEnd, static_cast<std::int64_t>(std::llround(anchor.sampleAt(initialBeat))));
        const auto start = nextSample;
        if (anchor.synced && havePrevious && resuming)
        {
            beatBias = anchor.beatAt(static_cast<double>(start)) - header.ppq;
            sampleBias = static_cast<double>(start) - static_cast<double>(header.samplePosition);
        }
        if (anchor.synced && havePrevious && header.playing && ! resuming)
        {
            const double wrapBeats = wrapAmount(header);
            if (wrapBeats > 0)
            {
                beatBias += wrapBeats;
                sampleBias = static_cast<double>(start) - static_cast<double>(header.samplePosition);
                // A musical loop can contain a fractional number of samples.
                // Hosts round its boundary on each pass; that sub-sample error
                // must not accumulate against the initial fixed anchor. The
                // wrap was verified independently by inspect(). Keep every raw
                // sample and start a fresh continuity anchor for this cycle.
                anchor.sample = start;
                anchor.ppq = header.ppq + beatBias;
            }
        }
        nextSample += header.sampleCount;
        previous = header; havePrevious = true;
        return start;
    }
    // Align a newly admitted source to the reference's cycle epoch. Host system
    // timestamps disambiguate short cycles; no message-thread arrival times are used.
    double initialBeatNear(const CaptureHeader& header) const noexcept
    {
        if (! anchor.synced || ! havePrevious) return header.ppq;
        if (! validLoop(header)) return header.ppq + beatBias;
        double target = anchor.beatAt(static_cast<double>(nextSample - previous.sampleCount));
        if ((header.valid & previous.valid & hasHostTime) != 0)
            target += (static_cast<double>(header.hostTimeNs) - static_cast<double>(previous.hostTimeNs)) * 1.0e-9 * anchor.bpm / 60;
        return header.ppq + std::round((target - header.ppq) / loopLength(header)) * loopLength(header);
    }
    const TimingAnchor& timing() const noexcept { return anchor; }
    bool initialized() const noexcept { return havePrevious; }
    bool paused() const noexcept { return havePrevious && (previous.valid & hasPosition) != 0 && ! previous.playing; }
private:
    double wrapAmount(const CaptureHeader& header) const noexcept
    {
        if (! previous.looping || ! validLoop(previous)) return 0;
        const double scale = 60.0 * supportedSampleRate / anchor.bpm;
        const double expected = previous.ppq + previous.sampleCount / scale;
        if (expected < previous.loopEndPpq - sampleTolerance / scale) return 0;
        const double amount = std::floor((expected - previous.loopStartPpq + sampleTolerance / scale)
            / loopLength(previous)) * loopLength(previous);
        return amount > 0 && std::abs((header.ppq - (expected - amount)) * scale) <= sampleTolerance ? amount : 0;
    }
    static double loopLength(const CaptureHeader& h) noexcept { return h.loopEndPpq - h.loopStartPpq; }
    static bool validLoop(const CaptureHeader& h) noexcept
    { return (h.valid & hasLoopBounds) != 0 && std::isfinite(h.loopStartPpq)
        && std::isfinite(h.loopEndPpq) && loopLength(h) > 0; }
    static bool hasSync(const CaptureHeader& header) noexcept
    {
        constexpr auto required = hasPosition | hasSamples | hasPpq | hasBpm;
        // Keep integer sample coordinates exactly representable and leave room
        // for a complete packet/history without signed overflow.
        return (header.valid & required) == required && std::isfinite(header.ppq)
            && std::isfinite(header.bpm) && header.bpm > 0
            && std::abs(static_cast<double>(header.samplePosition)) < 9.0e15;
    }
    TimingAnchor anchor;
    CaptureHeader previous;
    std::int64_t nextSample = 0;
    double beatBias = 0, sampleBias = 0;
    bool havePrevious = false;
};
}
