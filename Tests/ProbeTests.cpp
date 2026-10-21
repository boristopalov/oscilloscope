#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

// Check C++ heap allocations on the calling thread only, around warmed audio callbacks.
thread_local bool countAllocations = false;
thread_local std::size_t allocations = 0;
void* operator new(std::size_t bytes)
{
    if (countAllocations) ++allocations;
    if (auto* pointer = std::malloc(bytes == 0 ? 1 : bytes)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }

namespace
{
using namespace oscilloscope;
void require(bool condition, const char* description)
{
    if (! condition) throw std::runtime_error(description);
}

class TestPlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        return available ? juce::Optional<PositionInfo>(position) : juce::Optional<PositionInfo>();
    }
    bool available = true;
    PositionInfo position;
};

class ViewerLease
{
public:
    explicit ViewerLease(ProbeService& sharedService) : service(sharedService) {}
    ~ViewerLease() { while (count > 0) close(); }
    void open() { service.openViewer(); ++count; }
    void close() { service.closeViewer(); --count; }
private:
    ProbeService& service;
    int count = 0;
};

template <typename Sample>
void checkPassthrough(ScopeProcessor& processor, TestPlayHead& playHead, double rate, int size)
{
    processor.prepareToPlay(rate, size);
    juce::AudioBuffer<Sample> buffer(2, size);
    std::vector<Sample> before[2];
    const Sample values[] = { Sample(0), Sample(-0.0), Sample(1.5), Sample(-2.5),
        std::numeric_limits<Sample>::quiet_NaN(), std::numeric_limits<Sample>::infinity(),
        -std::numeric_limits<Sample>::infinity(), Sample(0.123456789) };
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int n = 0; n < size; ++n) buffer.setSample(channel, n, values[(n + channel) % 8]);
        before[channel].assign(buffer.getReadPointer(channel), buffer.getReadPointer(channel) + size);
    }
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(99)), 0);
    const auto midiSize = midi.getNumEvents();
    playHead.position.setTimeInSamples(1000);
    playHead.position.setPpqPosition(1000.0 / 24000.0);
    playHead.position.setBpm(120);
    playHead.position.setIsPlaying(true);
    // Warm any host/JUCE initialization before measuring callback allocations.
    processor.processBlock(buffer, midi);
    allocations = 0;
    countAllocations = true;
    for (int n = 0; n < 100; ++n) processor.processBlock(buffer, midi);
    countAllocations = false;
    require(allocations == 0, "Audio callback allocated C++ heap memory");
    for (int channel = 0; channel < 2; ++channel)
        require(std::memcmp(before[channel].data(), buffer.getReadPointer(channel),
                            static_cast<std::size_t>(size) * sizeof(Sample)) == 0,
                "Audio passthrough was not bit-identical");
    require(midi.getNumEvents() == midiSize, "MIDI buffer changed");
    require(processor.getLatencySamples() == 0, "Processor declared latency");
    for (int n = 0; n < 8; ++n) processor.probeService().drain();
}

void queueTests()
{
    SpscQueue<ProbeRecord, 4> small;
    ProbeRecord record, out;
    for (std::uint64_t n = 1; n <= 3; ++n) { record.sequence = n; require(small.push(record), "Queue filled early"); }
    require(! small.push(record), "Full queue overwrote owned slots");
    for (std::uint64_t n = 1; n <= 3; ++n)
        require(small.pop(out) && out.sequence == n, "Queue changed record order/content");
    require(! small.pop(out), "Empty queue returned data");

    SpscQueue<ProbeRecord, 64> concurrent;
    constexpr std::uint64_t count = 100000;
    std::atomic<bool> corrupted { false };
    std::thread producer([&]
    {
        for (std::uint64_t n = 1; n <= count; ++n)
        {
            ProbeRecord row;
            row.sequence = n;
            row.samplePosition = static_cast<std::int64_t>(n * 32);
            row.ppq = static_cast<double>(n);
            while (! concurrent.push(row)) std::this_thread::yield();
        }
    });
    for (std::uint64_t n = 1; n <= count; ++n)
    {
        while (! concurrent.pop(out)) std::this_thread::yield();
        if (out.sequence != n || out.samplePosition != static_cast<std::int64_t>(n * 32)
            || ! juce::exactlyEqual(out.ppq, static_cast<double>(n))) corrupted.store(true);
    }
    producer.join();
    require(! corrupted.load(), "Concurrent SPSC publication corrupted records");
}

ProbeRecord timingRecord(std::uint64_t sequence, std::int64_t sample)
{
    ProbeRecord r;
    r.sequence = sequence;
    r.generation = 1;
    r.valid = hasPosition | hasSamples | hasPpq | hasBpm;
    r.samplePosition = sample;
    r.ppq = static_cast<double>(sample) / 24000.0;
    r.bpm = 120;
    r.blockSize = 128;
    r.playing = true;
    return r;
}

void continuityTests()
{
    ContinuityProbe probe;
    require(probe.observe(timingRecord(1, 0)).event == TimingEvent::initial, "Anchor missing");
    auto row = probe.observe(timingRecord(2, 128));
    require(row.event == TimingEvent::steady && std::abs(row.ppqErrorInSamples) < 1.0e-9,
            "Fixed-tempo timestamps misclassified");
    require(probe.observe(timingRecord(3, 96000)).event == TimingEvent::discontinuity, "Seek missed");
    auto changed = timingRecord(4, 96128); changed.bpm = 121;
    require(probe.observe(changed).event == TimingEvent::tempoChanged, "Tempo edit missed");
    changed.sequence = 5; changed.looping = true;
    require(probe.observe(changed).event == TimingEvent::looping, "Loop guard missed");
    changed.sequence = 6; changed.looping = false; changed.playing = false;
    require(probe.observe(changed).event == TimingEvent::stopped, "Stop missed");
    changed.sequence = 7; changed.valid &= ~hasPpq;
    require(probe.observe(changed).event == TimingEvent::unavailable, "Missing field invented");
    probe = {};
    probe.observe(timingRecord(1, 0));
    require(probe.observe(timingRecord(3, 256)).event == TimingEvent::captureGap, "Capture gap called seek");
    auto fresh = timingRecord(4, 999); fresh.generation = 2;
    require(probe.observe(fresh).event == TimingEvent::initial, "Reset stitched generations");
    probe = {};
    probe.observe(timingRecord(1, 0));
    auto rounded = timingRecord(2, 128); rounded.ppq += 0.25 / 24000.0;
    require(probe.observe(rounded).event == TimingEvent::steady, "Subsample PPQ rounding rejected");
}

void processorTests()
{
    auto& service = ProbeService::instance();
    require(service.sources().empty(), "Initial registry not empty");
    ScopeProcessor first;
    auto second = std::make_unique<ScopeProcessor>();
    const auto survivor = second->probeSource();
    require(first.probeSource()->id != survivor->id, "Runtime identities collided");
    require(service.sources().size() == 2, "Registry failed cross-instance discovery");
    TestPlayHead playHead;
    first.setPlayHead(&playHead);
    ViewerLease viewers(service);
    viewers.open();
    require(service.isRecording(), "First viewer did not enable diagnostics");
    require(first.supportsDoublePrecisionProcessing(), "Double precision unsupported");
    for (double rate : { 44100.0, 48000.0, 96000.0 })
        for (int size : { 32, 64, 128, 256, 512, 1024, 2048 })
        {
            service.restart();
            checkPassthrough<float>(first, playHead, rate, size);
            checkPassthrough<double>(first, playHead, rate, size);
            const auto& rows = first.probeSource()->observations;
            require(rate == 48000.0 ? ! rows.empty() : rows.empty(), "Capture rate gate incorrect");
            if (! rows.empty())
            {
                const auto& r = rows.back().record;
                require(r.nonFinite > 0 && r.peakL == 2.5 && r.doublePrecision,
                        "Finite diagnostic peaks or precision flag incorrect");
            }
        }

    // A tap with no editor contributes to the same shared collection.
    service.restart();
    second->prepareToPlay(48000, 32);
    second->setPlayHead(&playHead);
    juce::AudioBuffer<float> buffer(2, 32); buffer.clear();
    buffer.setSample(0, 7, 1.5f);
    juce::MidiBuffer midi;
    second->processBlock(buffer, midi);
    service.drain();
    require(survivor->observations.size() == 1 && survivor->observations.back().record.peakOffsetL == 7,
            "Tap with unopened editor was not captured");

    // Old-generation records are rejected without resetting live FIFO indices.
    first.prepareToPlay(48000, 32);
    first.processBlock(buffer, midi);
    service.restart();
    first.processBlock(buffer, midi);
    service.drain();
    require(first.probeSource()->observations.size() == 1, "Reset admitted an old packet");

    // Saturation is visible without a later packet, and cannot change the audible samples.
    service.restart();
    const auto losses = first.probeSource()->dropped.load();
    for (int n = 0; n < 4096; ++n) first.processBlock(buffer, midi);
    require(first.probeSource()->dropped.load() == losses + 1, "Full queue did not report loss immediately");
    require(buffer.getSample(0, 7) == 1.5f, "Saturation changed audio");
    service.restart();
    for (int n = 0; n < 8; ++n) service.drain();
    first.processBlock(buffer, midi);
    service.drain();
    require(first.probeSource()->observations.size() == 1, "Saturated queue did not recover after new session");
    require(first.probeSource()->dropped.load() - first.probeSource()->droppedAtSessionStart == 0,
            "Old capture loss contaminated a clean diagnostic session");

    // Retention remains bounded, including long probe sessions.
    service.restart();
    for (std::size_t n = 0; n < ProbeService::maxRetainedRecords + 10; ++n)
    {
        first.processBlock(buffer, midi);
        if (n % 256 == 255) service.drain();
    }
    service.drain();
    require(first.probeSource()->observations.size() == ProbeService::maxRetainedRecords
            && first.probeSource()->evicted == 10, "Telemetry retention is unbounded or miscounted");

    playHead.available = false;
    first.processBlock(buffer, midi); service.drain();
    require(first.probeSource()->observations.back().event == TimingEvent::unavailable,
            "Absent host timing claimed synchronization");
    // JUCE's macOS tempDirectory is ~/Library/Caches, outside this workspace's
    // write sandbox. Keep test outputs beside the build instead.
    const auto csv = juce::File::getCurrentWorkingDirectory()
                         .getNonexistentChildFile("oscilloscope-probe-test", ".csv");
    require(service.exportCsv(csv), "CSV export failed");
    const auto contents = csv.loadFileAsString();
    require(contents.startsWith("schema,process_id,registry_token") && contents.contains("sync_unavailable"),
            "Export missing diagnostic evidence");
    require(csv.deleteFile(), "Could not clean up test CSV");

    viewers.open();
    viewers.close();
    require(service.isRecording(), "Closing second viewer stopped first");
    viewers.close();
    require(! service.isRecording(), "Last viewer did not disable diagnostics");
    const auto sequence = first.probeSource()->observations.back().record.sequence;
    first.processBlock(buffer, midi); service.drain();
    require(first.probeSource()->observations.back().record.sequence == sequence, "Closed viewers kept recording");
    viewers.open();
    require(first.probeSource()->observations.empty(), "Reopen retained previous session");
    viewers.close();

    second.reset();
    require(service.sources().size() == 1 && survivor->id != 0,
            "Source teardown failed or invalidated strong snapshot handle");
}
}

int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juce;
        queueTests(); continuityTests(); processorTests();
        require(oscilloscope::ProbeService::instance().sources().empty(), "Processor leaked registry entry");
        std::cout << "PASS: queue publication, continuity, bit-identical float/double audio, no callback C++ allocations, "
                     "48 kHz gate, discovery, bounded retention, CSV, reset, and lifecycle\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        countAllocations = false;
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
