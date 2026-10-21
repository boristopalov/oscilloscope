#pragma once
#include "DisplayService.h"
#include "ChannelProjector.h"
#include <algorithm>
#include <unordered_set>

namespace oscilloscope
{
enum class BeatWindow { quarterBeat, beat, bar, twoBars, fourBars };
enum class ViewLayout { stacked, overlay };
struct ViewWindow
{
    double begin = 0, end = 0, seconds = 0, barOrigin = 0, beatsPerBar = 4;
    bool synced = false, historyLimited = false, assumedBarOrigin = false, assumedMeter = false;
};
struct ViewLane
{
    std::uint64_t id = 0;
    juce::String name;
    std::uint32_t colour = 0;
    CaptureStatus status = CaptureStatus::waiting;
    TimingAnchor anchor;
    double offsetSamples = 0;
    double clipThresholdDb = 0;
    double frozenEnd = std::numeric_limits<double>::quiet_NaN(); // Independent frozen camera, including blank time.
    float clipAmplitude() const noexcept { return static_cast<float>(std::pow(10.0, clipThresholdDb / 20)); }
    bool synced = false;
    std::int64_t begin = 0, end = 0;
    const SourceHistory* history = nullptr;
    std::shared_ptr<const SourceState> sourceHandle; // Keep borrowed live history alive during a paint/copy.
    const StereoSample* snapshot = nullptr;
    const SampleExtrema* snapshotExtrema = nullptr;
    StereoSample at(std::int64_t sample) const noexcept
    {
        if (sample < begin || sample >= end) return {};
        return history != nullptr ? history->at(sample) : snapshot[static_cast<std::size_t>(sample - begin)];
    }
    bool hasSample(std::int64_t sample) const noexcept
    { return sample >= begin && sample < end && (history ? history->hasSample(sample) : std::isfinite(at(sample).left)); }
    SampleExtrema extrema(std::int64_t first, std::int64_t last) const noexcept
    {
        if (history != nullptr) return history->extrema(first, last);
        SampleExtrema result;
        first = std::max(first, begin); last = std::min(last, end);
        while (first < last)
        {
            const auto index = static_cast<std::size_t>(first - begin);
            if (snapshotExtrema != nullptr && index % SourceHistory::summarySamples == 0
                && last - first >= static_cast<std::int64_t>(SourceHistory::summarySamples))
            { result.merge(snapshotExtrema[index / SourceHistory::summarySamples]); first += static_cast<std::int64_t>(SourceHistory::summarySamples); }
            else { if (hasSample(first)) result.add(at(first)); ++first; }
        }
        return result;
    }
};

// Per-editor settings and freeze ownership. Never a queue consumer.
class ViewerModel
{
public:
    explicit ViewerModel(DisplayService& shared) : service(shared) {}
    ~ViewerModel() { unfreeze(); }
    ViewerModel(const ViewerModel&) = delete;
    ViewerModel& operator=(const ViewerModel&) = delete;
    void setWindow(BeatWindow choice) noexcept { choiceOfWindow = choice; }
    BeatWindow selectedWindow() const noexcept { return choiceOfWindow; }
    void setVisible(std::uint64_t id, bool visible)
    { if (visible) hidden.erase(id); else hidden.insert(id); }
    bool visible(std::uint64_t id) const { return hidden.count(id) == 0; }
    void setChannelMode(ChannelMode mode) noexcept { channels = mode; }
    ChannelMode channelMode() const noexcept { return channels; }
    void setLayout(ViewLayout value) noexcept { layout = value; }
    ViewLayout selectedLayout() const noexcept { return layout; }
    void setHeadroomDb(double value) noexcept
    { if (std::isfinite(value)) headroomDb = std::clamp(value, 0.0, 24.0); }
    double selectedHeadroomDb() const noexcept { return headroomDb; }
    ViewWindow window(double presentationTime = 0);
    std::vector<ViewLane> lanes() const;
    bool freeze();
    void unfreeze();
    bool frozen() const noexcept { return ! snapshots.empty(); }
    const juce::String& freezeMessage() const noexcept { return message; }
    static double beatSpan(BeatWindow, double beatsPerBar) noexcept;
private:
    struct FrozenLane
    {
        ViewLane lane;
        std::unique_ptr<StereoSample[]> samples;
        std::unique_ptr<SampleExtrema[]> extrema;
    };
    DisplayService& service;
    BeatWindow choiceOfWindow = BeatWindow::bar;
    std::unordered_set<std::uint64_t> hidden;
    ChannelMode channels = ChannelMode::leftRight;
    ViewLayout layout = ViewLayout::stacked;
    double headroomDb = 6;
    std::vector<FrozenLane> snapshots;
    ViewWindow lastWindow, frozenWindow;
    bool haveWindow = false, havePresentedWindow = false;
    ViewWindow presentedWindow;
    double lastPresentationTime = 0;
    std::size_t reservedBytes = 0;
    juce::String message;
};
}
