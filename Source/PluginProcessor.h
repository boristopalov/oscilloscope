#pragma once
#if OSCILLOSCOPE_DEV_PROBES
#include "ProbeService.h"
#endif
#include "DisplayService.h"
#include <juce_audio_processors/juce_audio_processors.h>

class ScopeProcessor final : public juce::AudioProcessor
{
public:
    ScopeProcessor();
    ~ScopeProcessor() override;
    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    bool supportsDoublePrecisionProcessing() const override { return true; }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    const juce::String getName() const override { return "Oscilloscope"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    void updateTrackProperties(const TrackProperties&) override;
#if OSCILLOSCOPE_DEV_PROBES
    oscilloscope::ProbeService& probeService() noexcept { return service; }
    const std::shared_ptr<oscilloscope::ProbeSource>& probeSource() const noexcept { return source; }
#endif
    oscilloscope::DisplayService& displayService() noexcept { return display; }
    const std::shared_ptr<oscilloscope::SourceState>& captureSource() const noexcept { return tap; }
private:
    template <typename Sample> void capture(const juce::AudioBuffer<Sample>&) noexcept;
    oscilloscope::DisplayService& display;
    const std::shared_ptr<oscilloscope::SourceState> tap;
    std::uint64_t captureSequence = 0; // Audio producer only.
#if OSCILLOSCOPE_DEV_PROBES
    oscilloscope::ProbeService& service;
    const std::shared_ptr<oscilloscope::ProbeSource> source;
    std::uint64_t sequence = 0; // Audio producer only; does not reset during prepare.
#endif
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScopeProcessor)
};
