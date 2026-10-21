#include "DisplayService.h"
#if OSCILLOSCOPE_DEV_PROBES
#include "ProbeService.h"
#endif

namespace oscilloscope
{
namespace
{
bool excluded(CaptureStatus status)
{
    return status == CaptureStatus::captureGap || status == CaptureStatus::stale
        || status == CaptureStatus::incompatible || status == CaptureStatus::unsupportedRate;
}
HoldReason timingHold(TimingResult result)
{
    switch (result)
    {
        case TimingResult::stopped: return HoldReason::stopped;
        case TimingResult::changed: return HoldReason::timingChanged;
        case TimingResult::lost: return HoldReason::timingLost;
        case TimingResult::synced:
        case TimingResult::unavailable: return HoldReason::none;
    }
    return HoldReason::none;
}
}
DisplayService& DisplayService::instance() { static DisplayService service; return service; }
void DisplayService::openViewer(std::uint64_t ownerId)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (viewers++ == 0)
    {
        // No callbacks were captured while every editor was closed. Do not use
        // old transport flags to decide the newly opened session's state.
        for (const auto& source : SourceRegistry::instance().sources()) source->haveTransport = false;
        reference = ownerId;
        if (reference == 0)
            for (const auto& source : SourceRegistry::instance().sources())
                if (source->admitted.load()) { reference = source->id; break; }
        reset();
        timer = std::make_unique<ConsumerTimer>(*this);
    }
}
void DisplayService::closeViewer()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    jassert(viewers > 0);
    if (--viewers == 0)
    {
        captureState.fetch_and(~std::uint64_t { 1 }, std::memory_order_release);
        timer.reset();
        for (int n = 0; n < 2; ++n) drain();
    }
}
void DisplayService::reset() { resetSession(); }
void DisplayService::resetSession()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto previous = captureState.fetch_and(~std::uint64_t { 1 }, std::memory_order_acq_rel);
    reason = HoldReason::none;
    sessionStartedMs = juce::Time::getMillisecondCounterHiRes();
    for (const auto& source : SourceRegistry::instance().sources())
    {
        source->summary = {};
        source->history.clear(); source->timeline.clear(); source->presentation.clear();
        source->lastPacketMs = sessionStartedMs;
        source->seenAdmitted = source->admitted.load(std::memory_order_acquire);
        source->rateRevisionAtReset = source->unsupportedRateRevision.load(std::memory_order_acquire);
        while (source->queue.consume([](const CapturePacket&) {})) {}
        if (! source->admitted.load()) continue;
        const auto rate = source->sampleRate.load(std::memory_order_acquire);
        if (! juce::exactlyEqual(rate, 0.0) && ! juce::exactlyEqual(rate, supportedSampleRate))
        { source->summary.status = CaptureStatus::unsupportedRate; hold(HoldReason::unsupportedRate); }

    }
    if (! referenceSource()) hold(HoldReason::referenceMissing);
    captureState.store((((previous >> 1) + 1) << 1) | (viewers > 0 ? 1u : 0u), std::memory_order_release);
    notify();
}
std::shared_ptr<SourceState> DisplayService::referenceSource() const
{
    for (const auto& source : SourceRegistry::instance().sources())
        if (source->id == reference && source->admitted.load()) return source;
    return {};
}
bool DisplayService::selectReference(std::uint64_t id)
{
    for (const auto& source : SourceRegistry::instance().sources())
        if (source->id == id && source->admitted.load())
        {
            if (reference != id) { reference = id; reason = HoldReason::referenceChanged; notify(); }
            return true;
        }
    return false;
}
bool DisplayService::setOffset(std::uint64_t id, double value, OffsetUnit unit)
{
    if (! std::isfinite(value) || std::abs(value * (unit == OffsetUnit::samples ? 1 : 48)) > SourceHistory::capacitySamples)
        return false;
    for (const auto& source : SourceRegistry::instance().sources())
        if (source->id == id)
        { source->offsetValue = value; source->offsetUnit = unit; notify(); return true; }
    return false;
}
bool DisplayService::setClipThreshold(std::uint64_t id, double db)
{
    if (! std::isfinite(db) || db < -60 || db > 24) return false;
    for (const auto& source : SourceRegistry::instance().sources())
        if (source->id == id) { source->clipThresholdDb = db; notify(); return true; }
    return false;
}
juce::String DisplayService::sessionMessage() const
{
    switch (reason)
    {
        case HoldReason::none: return "Live capture / Reset affects all live viewers";
        case HoldReason::timingChanged: return "Timing changed - Reset to resume";
        case HoldReason::timingLost: return "Beat timing lost - Reset to resume";
        case HoldReason::stopped: return "Paused / capture resumes automatically on Play";
        case HoldReason::captureGap: return "Reference capture gap - Reset to resume";
        case HoldReason::referenceMissing: return "Reference unavailable - select a reference, then Reset";
        case HoldReason::referenceStale: return "No recent reference data - restore it or select another, then Reset";
        case HoldReason::referenceChanged: return "Reference changed - Reset to resume";
        case HoldReason::unsupportedRate: return "Visualization requires 48 kHz - restore rate, then Reset";
    }
    return {};
}
std::size_t DisplayService::visualizationBytes() const
{
    std::size_t bytes = snapshotBytes;
    for (const auto& source : SourceRegistry::instance().sources())
        bytes += sizeof(SourceState) + source->queue.allocatedBytes() + source->history.allocatedBytes();
#if OSCILLOSCOPE_DEV_PROBES
    for (const auto& source : ProbeService::instance().sources())
        // Include conservative deque/container overhead for the diagnostic build.
        bytes += sizeof(ProbeSource) + (source->observations.size() + 64) * (sizeof(Observation) + 16);
#endif
    return bytes;
}
bool DisplayService::reserveSnapshot(std::size_t bytes)
{
    // Reserve the future full live histories too; freezing early must not allow
    // subsequent ingestion to exceed the cap as the eight histories fill.
    std::size_t worstLive = SourceRegistry::sourceLimit * (sizeof(SourceState) + CaptureQueue::storageBytes()
        + SourceHistory::capacitySamples * sizeof(StereoSample) + SourceHistory::maximumSummaryBytes());
    worstLive += SourceRegistry::sourceLimit * SourceHistory::maxCachedColumns * sizeof(SampleExtrema);
#if OSCILLOSCOPE_DEV_PROBES
    worstLive += SourceRegistry::sourceLimit * sizeof(ProbeSource);
    worstLive += SourceRegistry::sourceLimit * (ProbeService::maxRetainedRecords + 64) * (sizeof(Observation) + 16);
#endif
    const auto allocated = std::max(visualizationBytes() - snapshotBytes, worstLive) + snapshotBytes;
    if (allocated > visualizationBudgetBytes || bytes > visualizationBudgetBytes - allocated) return false;
    snapshotBytes += bytes; return true;
}
void DisplayService::releaseSnapshot(std::size_t bytes)
{ jassert(bytes <= snapshotBytes); snapshotBytes -= bytes; }

void DisplayService::drain(double nowMs)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto state = captureState.load(std::memory_order_acquire);
    const auto generation = state >> 1;
    const bool enabled = (state & 1u) != 0;
    auto sources = SourceRegistry::instance().sources();
    const auto ref = referenceSource();
    if (enabled && ! ref) hold(HoldReason::referenceMissing);
    bool clearForRate = false;
    // Inspect losses before ingesting ANY source; reference loss holds every
    // history even when the reference comes last in round-robin order.
    for (const auto& source : sources)
    {
        if (! source->admitted.load()) { source->seenAdmitted = false; continue; }
        if (! enabled) continue;
        if (! source->seenAdmitted)
        {
            // A newly added/admitted tap gets a full freshness grace period,
            // even when this session has already been playing for minutes.
            source->seenAdmitted = true; source->lastPacketMs = nowMs;
            source->rateRevisionAtReset = source->unsupportedRateRevision.load(std::memory_order_acquire);
        }
        if (source->lostGeneration.load(std::memory_order_acquire) == generation && generation != 0)
        {
            source->summary.status = CaptureStatus::captureGap;
            if (source == ref) hold(HoldReason::captureGap);
        }
        const auto rate = source->sampleRate.load(std::memory_order_acquire);
        if ((! juce::exactlyEqual(rate, 0.0) && ! juce::exactlyEqual(rate, supportedSampleRate))
            || source->unsupportedRateRevision.load(std::memory_order_acquire) != source->rateRevisionAtReset)
        {
            clearForRate = true; source->summary.status = CaptureStatus::unsupportedRate;
            hold(HoldReason::unsupportedRate);
        }
    }
    if (clearForRate) for (const auto& source : sources) source->history.clear();
    if (! sources.empty())
    {
        std::rotate(sources.begin(), sources.begin() + static_cast<std::ptrdiff_t>(roundRobin % sources.size()), sources.end());
        std::stable_sort(sources.begin(), sources.end(), [&](const auto& a, const auto& b)
            { return (a == ref) > (b == ref); });
    }
    // Interleave packets across sources rather than draining one complete FIFO
    // ahead of the others. The reference establishes each cycle epoch first.
    for (int budget = 0; budget < packetsPerSourcePerTick; ++budget)
        for (const auto& sourceHandle : sources)
        {
            auto& source = *sourceHandle;
            auto& summary = source.summary;
            source.queue.consume([&](const CapturePacket& packet)
            {
                if (packet.header.generation != generation || ! enabled || ! source.admitted.load()) return;
                source.observedTransport = packet.header; source.haveTransport = true;
                source.lastPacketMs = nowMs;
                if (held() || excluded(summary.status)) return;
                if (source.lostGeneration.load(std::memory_order_acquire) == generation
                    || (summary.packets != 0 && packet.header.sequence != summary.latest.header.sequence + 1))
                {
                    summary.status = CaptureStatus::captureGap;
                    if (source.id == reference) hold(HoldReason::captureGap);
                    return;
                }
                const auto result = source.timeline.inspect(packet.header);
                const auto error = timingHold(result);
                if (error != HoldReason::none) { hold(error); return; }
                const auto initialBeat = ref && ref->id != source.id
                    ? ref->timeline.initialBeatNear(packet.header) : std::numeric_limits<double>::quiet_NaN();
                const auto start = source.timeline.accept(packet.header, result, initialBeat);
                source.history.append(packet, start);
                source.presentation.observe(static_cast<double>(start), packet.header.captureTime, nowMs * 0.001);
                summary.latest = packet;
                summary.samples += static_cast<std::uint64_t>(packet.header.sampleCount);
                ++summary.packets;
                summary.nonFinite += packet.header.nonFinite;
                summary.peakL = summary.peakR = 0;
                for (int i = 0; i < packet.header.sampleCount; ++i)
                {
                    summary.peakL = std::max(summary.peakL, std::abs(packet.left[static_cast<std::size_t>(i)]));
                    summary.peakR = std::max(summary.peakR, std::abs(packet.right[static_cast<std::size_t>(i)]));
                }
                summary.status = result == TimingResult::synced ? CaptureStatus::live : CaptureStatus::syncUnavailable;
            });
            // Still observe a drop racing ingestion without a following packet.
            if (enabled && source.lostGeneration.load(std::memory_order_acquire) == generation && generation != 0)
            {
                summary.status = CaptureStatus::captureGap;
                if (source.id == reference) hold(HoldReason::captureGap);
            }
        }
    if (enabled && ! held() && ref)
    {
        if (nowMs - ref->lastPacketMs >= staleAfterMs && ! ref->timeline.paused())
            hold(HoldReason::referenceStale);
        for (const auto& source : sources)
        {
            if (source == ref || ! source->admitted.load() || excluded(source->summary.status) || ref->timeline.paused()) continue;
            if (nowMs - std::max(source->lastPacketMs, sessionStartedMs) >= staleAfterMs
                && nowMs - ref->lastPacketMs < staleAfterMs)
            { source->summary.status = CaptureStatus::stale; continue; }
            const auto& a = source->timeline.timing();
            const auto& b = ref->timeline.timing();
            if (a.synced && b.synced && (std::abs(a.bpm - b.bpm) > TimelineMapper::tempoTolerance
                || std::abs(a.beatsPerBar - b.beatsPerBar) > 1.0e-9
                || std::abs(a.beatAt(static_cast<double>(source->history.endSample()))
                          - b.beatAt(static_cast<double>(ref->history.endSample()))) * 60 / b.bpm > maxDisplayLagSeconds))
                source->summary.status = CaptureStatus::incompatible;
        }
    }
    if (! sources.empty()) roundRobin = (roundRobin + 1) % sources.size();
#if OSCILLOSCOPE_DEV_PROBES
    ProbeService::instance().drain();
#endif
    notify();
}
}
#if OSCILLOSCOPE_DEV_PROBES
extern "C" __attribute__((visibility("default"))) unsigned int oscilloscopeCaptureSourceCount()
{ return static_cast<unsigned int>(oscilloscope::SourceRegistry::instance().sources().size()); }
extern "C" __attribute__((visibility("default"))) void oscilloscopeCaptureDrain()
{ oscilloscope::DisplayService::instance().drain(); }
extern "C" __attribute__((visibility("default"))) std::uint64_t oscilloscopeCapturedSamples(unsigned int index)
{
    const auto sources = oscilloscope::SourceRegistry::instance().sources();
    return index < sources.size() ? sources[index]->summary.samples : 0;
}
#endif
