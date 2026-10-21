#pragma once
#include "PluginProcessor.h"
#include "ViewerModel.h"
#include "WaveformRenderer.h"
#include "WaveformSurface.h"
#include "ScopeLookAndFeel.h"
#include <juce_gui_basics/juce_gui_basics.h>

class ScopeEditor final : public juce::AudioProcessorEditor, private oscilloscope::DisplayService::Listener
{
public:
    explicit ScopeEditor(ScopeProcessor&);
    ~ScopeEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool usingGpu() const noexcept { return gpuActive; }
    std::function<void(oscilloscope::WaveformSurface::Presentation)> onPresented;
    double presentationEnd() const noexcept { return frameWindow.end; }
    std::uint64_t skippedGpuFrames() const noexcept { return waveformSurface ? waveformSurface->skippedFrames() : 0; }
    std::uint64_t failedGpuFrames() const noexcept { return waveformSurface ? waveformSurface->failedFrames() : 0; }
private:
    struct SourceRow
    {
        std::uint64_t id = 0;
        juce::ToggleButton visible;
        juce::Label name, clipLabel;
        juce::Rectangle<int> bounds;
        juce::TextButton colour;
        juce::Slider clip;
    };
    struct TrackCanvas : juce::Component
    {
        std::function<void(juce::Graphics&)> onPaint;
        void paint(juce::Graphics& g) override { if (onPaint) onPaint(g); }
    };
    std::vector<std::uint64_t> trackIds() const;
    juce::Rectangle<int> trackBounds(std::uint64_t) const;
    void displayUpdated() override;
    void refreshUi(bool force);
    void present(double timestamp);
    void preparePlots(bool aligned, float scale, double timestamp);
    struct Plot
    {
        std::vector<std::size_t> indices;
        std::vector<oscilloscope::WaveformFrame> frames;
        juce::Rectangle<float> area;
        float range = 2;
        bool overlay = false;
    };
    std::vector<Plot> plots;
    std::vector<oscilloscope::ViewLane> frameLanes;
    oscilloscope::ViewWindow frameWindow;
    std::unique_ptr<oscilloscope::WaveformSurface> waveformSurface;
    std::unique_ptr<juce::VBlankAttachment> vblank;
    std::unique_ptr<oscilloscope::FramePacer> pacer;
    bool gpuActive = false;
    double lastUiRefreshMs = 0, presentationTime = 0;
    void rebuildSources();
    void resizeToFitTracks();
    void paintWaveforms(juce::Graphics&);
    ScopeLookAndFeel lookAndFeel;
    TrackCanvas trackCanvas;
    juce::Viewport trackViewport;
    static constexpr int trackRailWidth = 208;
    int trackHeight = 80;
    bool automaticallySized = true, adjustingSize = false;
    juce::Point<int> automaticSize;
    std::vector<std::uint64_t> trackOrder;
    ScopeProcessor& owner;
    oscilloscope::ViewerModel view;
    juce::Label heading, status, headroomLabel;
    juce::ComboBox referenceSelector, windowSelector, channelSelector, focusSelector;
    juce::Slider headroomSlider;
    juce::TextButton resetButton { "Reset capture" };
    juce::TextButton layoutButton { "Stacked" }, freezeButton { "Freeze" };
    std::vector<std::unique_ptr<SourceRow>> rows;
    std::vector<std::uint64_t> referenceChoices;
    std::uint64_t focusedSource = 0;
    juce::Rectangle<int> waveformBounds;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScopeEditor)
};
