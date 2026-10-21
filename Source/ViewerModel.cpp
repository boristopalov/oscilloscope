#include "ViewerModel.h"

namespace oscilloscope
{
namespace
{
bool comparable(CaptureStatus status) { return status == CaptureStatus::live; }
}
double ViewerModel::beatSpan(BeatWindow choice, double beatsPerBar) noexcept
{
    switch (choice)
    {
        case BeatWindow::quarterBeat: return 0.25;
        case BeatWindow::beat: return 1;
        case BeatWindow::bar: return beatsPerBar;
        case BeatWindow::twoBars: return 2 * beatsPerBar;
        case BeatWindow::fourBars: return 4 * beatsPerBar;
    }
    return beatsPerBar;
}
std::vector<ViewLane> ViewerModel::lanes() const
{
    std::vector<ViewLane> result;
    if (frozen())
    {
        const auto sources = SourceRegistry::instance().sources();
        for (const auto& snapshot : snapshots)
        {
            auto lane = snapshot.lane;
            // Freeze owns samples/timing, while names and colors remain shared.
            // If a tap was removed, retain the last copied presentation metadata.
            for (const auto& source : sources)
                if (source->id == lane.id) { lane.name = source->displayName(); lane.colour = source->colour;
                    lane.clipThresholdDb = source->clipThresholdDb; break; }
            result.push_back(std::move(lane));
        }
        return result;
    }
    const auto ref = service.referenceSource();
    for (const auto& source : SourceRegistry::instance().sources())
    {
        if (! source->admitted.load() || ! visible(source->id)) continue;
        ViewLane lane;
        lane.id = source->id; lane.status = source->summary.status;
        lane.name = source->displayName(); lane.colour = source->colour;
        lane.clipThresholdDb = source->clipThresholdDb;
        lane.anchor = source->timeline.timing(); lane.offsetSamples = source->offsetSamples();
        lane.synced = lane.anchor.synced && (comparable(lane.status)
            || (service.held() && source->id == service.referenceId()))
            && ((ref && ref->timeline.timing().synced) || (service.held() && lastWindow.synced));
        lane.begin = source->history.beginSample(); lane.end = source->history.endSample();
        lane.history = &source->history;
        lane.sourceHandle = source;
        result.push_back(lane);
    }
    return result;
}
ViewWindow ViewerModel::window(double presentationTime)
{
    if (frozen()) return frozenWindow;
    if (service.held() && havePresentedWindow) return presentedWindow;
    if (service.held() && haveWindow) return lastWindow;
    const auto ref = service.referenceSource();
    const auto anchor = ref ? ref->timeline.timing() : TimingAnchor {};
    ViewWindow result;
    result.synced = ref && anchor.synced && ref->history.size() != 0;
    result.barOrigin = anchor.barOrigin; result.beatsPerBar = anchor.beatsPerBar;
    result.assumedBarOrigin = anchor.assumedBarOrigin; result.assumedMeter = anchor.assumedMeter;
    const double span = beatSpan(choiceOfWindow, anchor.beatsPerBar);
    result.seconds = span * 60 / anchor.bpm;
    result.historyLimited = result.seconds > 16;
    if (result.synced)
    {
        const auto refEnd = anchor.beatAt(static_cast<double>(ref->history.endSample()), ref->offsetSamples());
        result.end = refEnd;
        for (const auto& lane : lanes())
            if (lane.synced && lane.end > lane.begin)
                result.end = std::min(result.end, lane.anchor.beatAt(static_cast<double>(lane.end), lane.offsetSamples));
        // A manual offset must not force an unbounded global display lag.
        result.end = std::max(result.end, refEnd - DisplayService::maxDisplayLagSeconds * anchor.bpm / 60);
        if (presentationTime > 0 && ref->presentation.initialized())
            result.end = anchor.beatAt(ref->presentation.cursorAt(presentationTime), ref->offsetSamples());
        result.begin = result.end - span;
    }
    else { result.end = 0; result.begin = -result.seconds; }
    if (presentationTime > 0) { presentedWindow = result; havePresentedWindow = true; lastPresentationTime = presentationTime; }
    lastWindow = result;
    haveWindow = true;
    return result;
}
bool ViewerModel::freeze()
{
    if (frozen()) return true;
    const auto selectedWindow = havePresentedWindow ? presentedWindow : window();
    auto selectedLanes = lanes();
    std::size_t bytes = 0;
    for (auto& lane : selectedLanes)
    {
        if (lane.synced && selectedWindow.synced)
        {
            const auto first = lane.anchor.sampleAt(selectedWindow.begin, lane.offsetSamples);
            const auto last = lane.anchor.sampleAt(selectedWindow.end, lane.offsetSamples);
            // Clamp in floating point before converting to avoid overflow with
            // unavailable/very remote musical regions.
            lane.begin = static_cast<std::int64_t>(std::clamp(std::floor(first), static_cast<double>(lane.begin), static_cast<double>(lane.end)));
            lane.end = static_cast<std::int64_t>(std::clamp(std::ceil(last), static_cast<double>(lane.begin), static_cast<double>(lane.end)));
        }
        else
        {
            // Freeze the displayed independent camera, including any blank
            // trailing time; using the latest packet would jump by the delay.
            lane.frozenEnd = havePresentedWindow && lane.sourceHandle && lane.sourceHandle->presentation.initialized()
                ? lane.sourceHandle->presentation.cursorAt(lastPresentationTime) : static_cast<double>(lane.end);
            const auto first = lane.frozenEnd - std::min(16.0, selectedWindow.seconds) * supportedSampleRate;
            lane.begin = static_cast<std::int64_t>(std::clamp(std::floor(first), static_cast<double>(lane.begin), static_cast<double>(lane.end)));
            lane.end = static_cast<std::int64_t>(std::clamp(std::ceil(lane.frozenEnd), static_cast<double>(lane.begin), static_cast<double>(lane.end)));
        }
        const auto count = static_cast<std::size_t>(lane.end - lane.begin);
        bytes += count * sizeof(StereoSample)
            + ((count + SourceHistory::summarySamples - 1) / SourceHistory::summarySamples) * sizeof(SampleExtrema) + 1024;
    }
    if (selectedLanes.empty() || std::none_of(selectedLanes.begin(), selectedLanes.end(), [](const ViewLane& lane) { return lane.end > lane.begin; }))
    { message = "No samples available to freeze"; return false; }
    if (! service.reserveSnapshot(bytes))
    { message = "Freeze refused: 256 MiB visualization budget reached"; return false; }
    // Commit only after all allocations/copies succeed, and release reservation
    // on failure. No retained SourceState handles or FIFO storage in snapshots.
    try
    {
        std::vector<FrozenLane> pending;
        pending.reserve(selectedLanes.size());
        for (auto lane : selectedLanes)
        {
            FrozenLane snapshot;
            snapshot.samples = std::make_unique<StereoSample[]>(static_cast<std::size_t>(lane.end - lane.begin));
            snapshot.extrema = std::make_unique<SampleExtrema[]>((static_cast<std::size_t>(lane.end - lane.begin)
                + SourceHistory::summarySamples - 1) / SourceHistory::summarySamples);
            for (auto sample = lane.begin; sample < lane.end; ++sample)
            {
                snapshot.samples[static_cast<std::size_t>(sample - lane.begin)] = lane.history->at(sample);
                if (lane.hasSample(sample)) snapshot.extrema[static_cast<std::size_t>(sample - lane.begin) / SourceHistory::summarySamples].add(snapshot.samples[static_cast<std::size_t>(sample - lane.begin)]);
            }
            lane.history = nullptr; lane.sourceHandle.reset();
            lane.snapshot = snapshot.samples.get(); lane.snapshotExtrema = snapshot.extrema.get(); snapshot.lane = lane;
            pending.push_back(std::move(snapshot));
        }
        snapshots = std::move(pending); frozenWindow = selectedWindow;
        reservedBytes = bytes; message = "Frozen snapshot / Reset preserves this view";
        return true;
    }
    catch (const std::bad_alloc&)
    { service.releaseSnapshot(bytes); message = "Freeze refused: insufficient memory"; return false; }
}
void ViewerModel::unfreeze()
{
    snapshots.clear();
    if (reservedBytes != 0) service.releaseSnapshot(reservedBytes);
    reservedBytes = 0; havePresentedWindow = false; message.clear();
}
}
