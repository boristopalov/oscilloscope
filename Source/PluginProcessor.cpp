#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <type_traits>

using namespace oscilloscope;

ScopeProcessor::ScopeProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      display(DisplayService::instance()), tap(SourceRegistry::instance().registerSource())
#if OSCILLOSCOPE_DEV_PROBES
      , service(ProbeService::instance()), source(service.registerSource())
#endif
{
    setLatencySamples(0);
}

ScopeProcessor::~ScopeProcessor()
{
    SourceRegistry::instance().unregisterSource(tap);
#if OSCILLOSCOPE_DEV_PROBES
    service.unregisterSource(source);
#endif
}
void ScopeProcessor::prepareToPlay(double rate, int)
{
    if (! juce::exactlyEqual(rate, supportedSampleRate))
        tap->unsupportedRateRevision.fetch_add(1, std::memory_order_release);
    tap->sampleRate.store(rate, std::memory_order_release);
#if OSCILLOSCOPE_DEV_PROBES
    source->sampleRate.store(rate, std::memory_order_release);
#endif
}
bool ScopeProcessor::isBusesLayoutSupported(const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

template <typename Sample>
void ScopeProcessor::capture(const juce::AudioBuffer<Sample>& buffer) noexcept
{
    const auto captureSession = display.captureState.load(std::memory_order_acquire);
    const bool captureAudio = (captureSession & 1u) != 0 && tap->admitted.load(std::memory_order_acquire)
                          && juce::exactlyEqual(tap->sampleRate.load(std::memory_order_acquire), supportedSampleRate)
                          && buffer.getNumSamples() > 0 && buffer.getNumChannels() >= 2;
#if OSCILLOSCOPE_DEV_PROBES
    source->processedBlocks.fetch_add(1, std::memory_order_relaxed);
    const auto state = service.captureState.load(std::memory_order_acquire);
    const bool captureProbe = (state & 1u) != 0
                          && juce::exactlyEqual(source->sampleRate.load(std::memory_order_acquire), supportedSampleRate);
#else
    const bool captureProbe = false;
#endif
    if (! captureAudio && ! captureProbe) return;
    ProbeRecord record;
#if OSCILLOSCOPE_DEV_PROBES
    record.generation = state >> 1;
    record.sequence = ++sequence;
    record.blockSize = buffer.getNumSamples();
    record.doublePrecision = std::is_same_v<Sample, double>;
#endif
    CaptureHeader header;
    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            record.valid |= hasPosition;
            if (const auto samples = position->getTimeInSamples())
            { record.valid |= hasSamples; record.samplePosition = *samples; }
            if (const auto ppq = position->getPpqPosition())
            { record.valid |= hasPpq; record.ppq = *ppq; }
            if (const auto bpm = position->getBpm())
            { record.valid |= hasBpm; record.bpm = *bpm; }
            if (const auto bar = position->getPpqPositionOfLastBarStart())
            { record.valid |= hasBarStart; record.barStartPpq = *bar; }
            if (const auto seconds = position->getTimeInSeconds())
            { record.valid |= hasSeconds; record.seconds = *seconds; }
            if (const auto meter = position->getTimeSignature())
            { record.valid |= hasMeter; record.numerator = meter->numerator; record.denominator = meter->denominator; }
            record.playing = position->getIsPlaying();
            record.looping = position->getIsLooping();
            record.recording = position->getIsRecording();
            if (const auto loop = position->getLoopPoints())
            {
                if (std::isfinite(loop->ppqStart) && std::isfinite(loop->ppqEnd) && loop->ppqEnd > loop->ppqStart)
                {
                    header.valid |= hasLoopBounds;
                    header.loopStartPpq = loop->ppqStart; header.loopEndPpq = loop->ppqEnd;
                }
            }
            if (const auto time = position->getHostTimeNs())
            { header.valid |= hasHostTime; header.hostTimeNs = *time; }
        }
    }
    if (captureAudio)
    {
        header.captureTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
        header.generation = captureSession >> 1;
        header.sequence = captureSequence + 1;
        header.valid |= record.valid & (hasPosition | hasSamples | hasPpq | hasBpm | hasBarStart | hasMeter);
        header.samplePosition = record.samplePosition;
        header.ppq = record.ppq;
        header.bpm = record.bpm;
        header.barStartPpq = record.barStartPpq;
        header.numerator = record.numerator;
        header.denominator = record.denominator;
        header.playing = record.playing;
        header.looping = record.looping;
        captureSequence += static_cast<std::uint64_t>(1 + (buffer.getNumSamples() - 1) / captureChunkSamples);
        if (! tap->queue.push(buffer.getReadPointer(0), buffer.getReadPointer(1), buffer.getNumSamples(), header))
        {
            tap->droppedCallbacks.fetch_add(1, std::memory_order_relaxed);
            tap->lostGeneration.store(header.generation, std::memory_order_release);
        }
    }
#if OSCILLOSCOPE_DEV_PROBES
    if (! captureProbe) return;
    for (int channel = 0; channel < juce::jmin(2, buffer.getNumChannels()); ++channel)
    {
        const auto* samples = buffer.getReadPointer(channel);
        auto& peak = channel == 0 ? record.peakL : record.peakR;
        auto& offset = channel == 0 ? record.peakOffsetL : record.peakOffsetR;
        for (int n = 0; n < buffer.getNumSamples(); ++n)
        {
            const auto value = static_cast<double>(samples[n]);
            if (! std::isfinite(value)) { ++record.nonFinite; continue; }
            const auto magnitude = std::abs(value);
            if (magnitude > peak) { peak = magnitude; offset = n; }
        }
    }
    if (! source->queue.push(record)) source->dropped.fetch_add(1, std::memory_order_relaxed);
#endif
}

// No writes to audio or MIDI: including NaN/Inf, signed zero, and >0 dBFS samples.
void ScopeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) { capture(buffer); }
void ScopeProcessor::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) { capture(buffer); }
juce::AudioProcessorEditor* ScopeProcessor::createEditor() { return new ScopeEditor(*this); }

void ScopeProcessor::getStateInformation(juce::MemoryBlock& state)
{
    // No diagnostics, runtime IDs or session state are persisted. Full schema is milestone 5.
    const char version[] = "oscilloscope-probe-state-v1";
    state.replaceAll(version, sizeof(version));
}
void ScopeProcessor::setStateInformation(const void*, int) {}
void ScopeProcessor::updateTrackProperties(const TrackProperties& properties)
{
    // JUCE specifies this host callback runs on the message thread.
    if (properties.name) tap->hostName = properties.name->trim();
    if (! tap->colourOverridden && properties.colourARGB)
        tap->colour = *properties.colourARGB | 0xff000000u;
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ScopeProcessor(); }
