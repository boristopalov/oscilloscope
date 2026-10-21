#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

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
void require(bool condition, const char* message) { if (! condition) throw std::runtime_error(message); }
class PlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override { return position; }
    PositionInfo position;
    void advance(int samples)
    {
        position.setTimeInSamples(*position.getTimeInSamples() + samples);
        position.setPpqPosition(*position.getPpqPosition() + samples / 24000.0);
    }
};
struct Viewers
{
    DisplayService& service = DisplayService::instance();
    int count = 0;
    void open() { service.openViewer(); ++count; }
    void close() { service.closeViewer(); --count; }
    ~Viewers() { while (count > 0) close(); }
};
void flush(DisplayService& service) { service.drain(); service.drain(); }

void queueTests()
{
    auto queue = std::make_unique<CaptureQueue>();
    std::vector<float> left(4097), right(4097);
    for (int n = 0; n < 4097; ++n) { left[static_cast<std::size_t>(n)] = static_cast<float>(n); right[static_cast<std::size_t>(n)] = -static_cast<float>(n); }
    CaptureHeader header;
    header.generation = 1; header.sequence = 10;
    header.valid = hasSamples | hasPpq | hasBpm | hasBarStart;
    header.samplePosition = 100; header.ppq = 4; header.bpm = 120; header.barStartPpq = 4;
    header.playing = true;
    require(queue->push(left.data(), right.data(), 4097, header), "Split callback rejected");
    for (int chunk = 0; chunk < 3; ++chunk)
        require(queue->consume([&](const CapturePacket& packet)
        {
            const int offset = chunk * captureChunkSamples;
            require(packet.header.sequence == static_cast<std::uint64_t>(10 + chunk)
                    && packet.header.sampleCount == (chunk == 2 ? 1 : 2048)
                    && packet.header.samplePosition == 100 + offset
                    && std::abs(packet.header.ppq - (4 + offset / 24000.0)) < 1e-12
                    && packet.header.playing && packet.header.generation == 1, "Split timing metadata incorrect");
            for (int n = 0; n < packet.header.sampleCount; ++n)
                require(juce::exactlyEqual(packet.left[static_cast<std::size_t>(n)], left[static_cast<std::size_t>(offset + n)])
                        && juce::exactlyEqual(packet.right[static_cast<std::size_t>(n)], right[static_cast<std::size_t>(offset + n)]), "Raw stereo samples changed");
        }), "Split packet missing");
    auto missingTempo = header; missingTempo.valid = hasPpq;
    require(queue->push(left.data(), right.data(), 4097, missingTempo), "Missing-tempo packet rejected");
    for (int chunk = 0; chunk < 3; ++chunk)
        require(queue->consume([&](const CapturePacket& packet)
        {
            require(chunk == 0 ? (packet.header.valid & hasPpq) != 0 : (packet.header.valid & hasPpq) == 0,
                    "Split chunk invented PPQ without tempo");
        }), "Missing-tempo chunk absent");
    std::array<double, 4> extreme { 1.5, std::numeric_limits<double>::max(),
                                  std::numeric_limits<double>::infinity(), -0.0 };
    require(queue->push(extreme.data(), extreme.data(), 4, header), "Double display conversion rejected");
    queue->consume([](const CapturePacket& packet)
    {
        require(packet.header.nonFinite == 4 && juce::exactlyEqual(packet.left[0], 1.5f)
                && juce::exactlyEqual(packet.left[1], 0.0f) && juce::exactlyEqual(packet.left[2], 0.0f)
                && std::signbit(packet.left[3]), "Double conversion overflow/non-finite handling failed");
    });
    // Exercise wrap and whole-callback reservation: only one slot remains.
    for (int n = 0; n < 126; ++n) require(queue->push(left.data(), right.data(), 32, header), "Queue filled early");
    require(! queue->push(left.data(), right.data(), 4097, header) && queue->ready() == 126,
            "Insufficient callback reservation partially published or overwrote data");
    require(! queue->push(left.data(), right.data(), 128 * 2048, header), "Oversized callback accepted");
    require(queue->push(left.data(), right.data(), 32, header), "Last usable slot absent");
    require(! queue->push(left.data(), right.data(), 32, header), "Full queue accepted overwrite");
    while (queue->consume([](const CapturePacket&) {})) {}
    require(queue->ready() == 0, "Queue not empty after consumption");

    constexpr int total = 20000;
    std::atomic<bool> corrupted { false };
    std::thread producer([&]
    {
        std::array<float, 32> l {}, r {};
        for (int n = 1; n <= total; ++n)
        {
            l.fill(static_cast<float>(n)); r.fill(-static_cast<float>(n));
            CaptureHeader h; h.sequence = static_cast<std::uint64_t>(n);
            while (! queue->push(l.data(), r.data(), 32, h)) std::this_thread::yield();
        }
    });
    for (int n = 1; n <= total; ++n)
        while (! queue->consume([&](const CapturePacket& packet)
        {
            if (packet.header.sequence != static_cast<std::uint64_t>(n)) corrupted.store(true);
            for (int i = 0; i < 32; ++i)
                if (! juce::exactlyEqual(packet.left[static_cast<std::size_t>(i)], static_cast<float>(n))
                    || ! juce::exactlyEqual(packet.right[static_cast<std::size_t>(i)], -static_cast<float>(n))) corrupted.store(true);
        })) std::this_thread::yield();
    producer.join();
    require(! corrupted.load(), "Concurrent raw queue publication corrupted samples");
}

template <typename Sample>
void callbackTests(ScopeProcessor& processor, DisplayService& service, int size, double rate)
{
    processor.prepareToPlay(rate, size);
    service.reset();
    juce::AudioBuffer<Sample> buffer(2, size);
    const Sample values[] = { Sample(0), Sample(-0.0), Sample(1.5), Sample(-2.5),
        std::numeric_limits<Sample>::quiet_NaN(), std::numeric_limits<Sample>::infinity(),
        -std::numeric_limits<Sample>::infinity(), Sample(0.123456789) };
    for (int channel = 0; channel < 2; ++channel)
        for (int n = 0; n < size; ++n) buffer.setSample(channel, n, values[(n + channel) % 8]);
    juce::AudioBuffer<Sample> before; before.makeCopyOf(buffer);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(99)), 0);
    allocations = 0;
    countAllocations = true;
    processor.processBlock(buffer, midi);
    countAllocations = false;
    require(allocations == 0, "Raw capture allocated in callback");
    for (int channel = 0; channel < 2; ++channel)
        require(std::memcmp(before.getReadPointer(channel), buffer.getReadPointer(channel),
                            static_cast<std::size_t>(size) * sizeof(Sample)) == 0, "Raw capture changed audible sample bits");
    require(midi.getNumEvents() == 1 && processor.getLatencySamples() == 0, "MIDI/latency changed");
    service.drain();
    const auto& summary = processor.captureSource()->summary;
    if (juce::exactlyEqual(rate, supportedSampleRate))
    {
        require(summary.samples == static_cast<std::uint64_t>(size) && summary.nonFinite > 0, "Captured sample count or non-finite status incorrect");
        require(juce::exactlyEqual(summary.peakL, 2.5f) && juce::exactlyEqual(summary.latest.left[2], 1.5f)
                && juce::exactlyEqual(summary.latest.left[4], 0.0f), "Display conversion clamped peaks or retained NaN");
    }
    else require(summary.samples == 0 && summary.status == CaptureStatus::unsupportedRate, "48 kHz capture gate failed");
}

void processorLoopTransport()
{
    ScopeProcessor processor; processor.prepareToPlay(48000, 512);
    PlayHead playHead; processor.setPlayHead(&playHead);
    playHead.position.setBpm(120); playHead.position.setIsPlaying(true); playHead.position.setIsLooping(true);
    playHead.position.setLoopPoints(juce::AudioPlayHead::LoopPoints { 8, 8.1 });
    Viewers viewers; viewers.open();
    auto& service = processor.displayService();
    juce::AudioBuffer<float> buffer(2, 512); juce::MidiBuffer midi;
    int loopSample = 0, total = 0;
    for (int block = 0; block < 100; ++block)
    {
        const int count = std::min(512, 2400 - loopSample);
        buffer.setSize(2, count, false, false, true); buffer.clear(); buffer.setSample(0, count - 1, 1.5f);
        playHead.position.setTimeInSamples(192000 + loopSample);
        playHead.position.setPpqPosition(8 + loopSample / 24000.0);
        playHead.position.setHostTimeNs(1000000000 + static_cast<std::uint64_t>(total) * 1000000000 / 48000);
        allocations = 0; countAllocations = true; processor.processBlock(buffer, midi); countAllocations = false;
        require(allocations == 0, "Loop timing capture allocated on the audio callback");
        service.drain(); total += count; loopSample = (loopSample + count) % 2400;
        require(! service.held(), "Actual processor host loop metadata did not survive a cycle boundary");
    }
    const auto source = processor.captureSource(); const auto& header = source->summary.latest.header;
    require(source->summary.samples == static_cast<std::uint64_t>(total)
        && (header.valid & (hasLoopBounds | hasHostTime)) == (hasLoopBounds | hasHostTime)
        && std::abs(header.loopStartPpq - 8) < 1e-9 && std::abs(header.loopEndPpq - 8.1) < 1e-9,
        "Processor dropped loop samples or optional host loop/time metadata");
    require(std::abs(source->timeline.timing().beatAt(static_cast<double>(source->history.endSample()))
        - (8 + total / 24000.0)) < 1e-9, "Actual processor loop display did not advance continuously");
}
void serviceTests()
{
    auto& service = DisplayService::instance();
    auto& registry = SourceRegistry::instance();
    ScopeProcessor first;
    auto second = std::make_unique<ScopeProcessor>();
    PlayHead playHead;
    playHead.position.setBpm(120); playHead.position.setPpqPosition(0);
    playHead.position.setTimeInSamples(0); playHead.position.setIsPlaying(true);
    first.setPlayHead(&playHead); second->setPlayHead(&playHead);
    Viewers viewers;
    viewers.open();
    require(registry.sources().size() == 2 && first.captureSource()->id != second->captureSource()->id, "Two sources not discovered uniquely");
    for (int size : { 32, 64, 128, 256, 512, 1024, 2048 })
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        { callbackTests<float>(first, service, size, rate); callbackTests<double>(first, service, size, rate); }

    juce::MidiBuffer midi;
    // Simulate ten seconds at a 30 Hz display cadence for each supported callback.
    for (int size : { 32, 64, 128, 256, 512, 1024, 2048 })
    {
        first.prepareToPlay(48000, size); second->prepareToPlay(48000, size);
        juce::AudioBuffer<float> buffer(2, size); buffer.clear();
        buffer.setSample(0, size - 1, 1.5f); buffer.setSample(1, 0, -2.0f);
        service.reset();
        const auto losses = first.captureSource()->droppedCallbacks.load();
        const int blocks = 480000 / size;
        int nextTick = 1600;
        for (int n = 0; n < blocks; ++n)
        {
            // Callback order deliberately alternates between sources.
            if (n % 2 == 0) { first.processBlock(buffer, midi); second->processBlock(buffer, midi); }
            else { second->processBlock(buffer, midi); first.processBlock(buffer, midi); }
            playHead.advance(size);
            if ((n + 1) * size >= nextTick) { service.drain(); nextTick += 1600; }
        }
        flush(service);
        require(first.captureSource()->summary.samples == static_cast<std::uint64_t>(blocks * size)
                && second->captureSource()->summary.samples == static_cast<std::uint64_t>(blocks * size)
                && first.captureSource()->droppedCallbacks.load() == losses, "Normal 30 Hz two-source capture lost samples");
        // Empty queue: enqueue every callback covering a controlled 50 ms (2400 samples)
        // with NO consumer work. Deterministic capacity test independent of OS scheduling.
        service.reset();
        const int pausedBlocks = (2400 + size - 1) / size;
        for (int n = 0; n < pausedBlocks; ++n) { first.processBlock(buffer, midi); second->processBlock(buffer, midi); playHead.advance(size); }
        flush(service);
        require(first.captureSource()->summary.samples == static_cast<std::uint64_t>(pausedBlocks * size)
                && second->captureSource()->summary.samples == static_cast<std::uint64_t>(pausedBlocks * size)
                && first.captureSource()->droppedCallbacks.load() == losses, "50 ms paused consumer lost raw capture");
    }

    first.prepareToPlay(48000, 32); second->prepareToPlay(48000, 32);
    juce::AudioBuffer<float> buffer(2, 32); buffer.clear(); buffer.setSample(0, 7, 1.5f);
    service.reset();
    // Actual concurrent, paced audio producer during a 50 ms message-thread pause.
    std::atomic<bool> start { false };
    std::thread audio([&]
    {
        while (! start.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto begin = std::chrono::steady_clock::now();
        for (int n = 0; n < 75; ++n)
        {
            std::this_thread::sleep_until(begin + std::chrono::microseconds(n * 2000 / 3));
            first.processBlock(buffer, midi);
            playHead.advance(32);
        }
    });
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    audio.join(); flush(service);
    require(first.captureSource()->summary.samples == 2400, "Concurrent 50 ms consumer pause lost data");

    service.reset();
    const auto losses = first.captureSource()->droppedCallbacks.load();
    allocations = 0; countAllocations = true;
    for (int n = 0; n < 128; ++n) { first.processBlock(buffer, midi); playHead.advance(32); }
    countAllocations = false;
    require(allocations == 0 && first.captureSource()->droppedCallbacks.load() == losses + 1
            && juce::exactlyEqual(buffer.getSample(0, 7), 1.5f), "Saturation allocated, lost status, or changed audio");
    flush(service);
    require(first.captureSource()->summary.status == CaptureStatus::captureGap, "Loss without subsequent packet was invisible");
    first.processBlock(buffer, midi); flush(service);
    require(first.captureSource()->summary.samples == 0 && first.captureSource()->summary.status == CaptureStatus::captureGap,
            "Gap automatically resumed without Reset");
    second->processBlock(buffer, midi); service.drain();
    require(second->captureSource()->summary.samples == 0 && service.held(), "Reference gap failed to hold the shared view");
    service.reset(); first.processBlock(buffer, midi); service.drain();
    require(first.captureSource()->summary.samples == 32 && first.captureSource()->summary.status == CaptureStatus::live,
            "Reset failed saturation recovery");

    // Model a callback already in flight at Reset: publish with old generation later.
    const auto oldGeneration = service.captureState.load() >> 1;
    service.reset();
    CaptureHeader old; old.generation = oldGeneration; old.sequence = 999999;
    require(first.captureSource()->queue.push(buffer.getReadPointer(0), buffer.getReadPointer(1), 32, old), "Old packet setup failed");
    first.captureSource()->lostGeneration.store(oldGeneration);
    first.processBlock(buffer, midi); service.drain();
    require(first.captureSource()->summary.samples == 32 && first.captureSource()->summary.status == CaptureStatus::live,
            "In-flight old packet/loss contaminated Reset");
    // Sequence gap is detected independently of the loss counter.
    CaptureHeader gap; gap.generation = service.captureState.load() >> 1;
    gap.sequence = first.captureSource()->summary.latest.header.sequence + 2;
    first.captureSource()->queue.push(buffer.getReadPointer(0), buffer.getReadPointer(1), 32, gap);
    service.drain();
    require(first.captureSource()->summary.status == CaptureStatus::captureGap, "Packet sequence gap missed");

    std::vector<std::unique_ptr<ScopeProcessor>> extra;
    for (int n = 0; n < 6; ++n)
    { extra.push_back(std::make_unique<ScopeProcessor>()); extra.back()->prepareToPlay(48000, 32); }
    auto third = std::make_unique<ScopeProcessor>();
    third->prepareToPlay(48000, 32);
    require(! third->captureSource()->admitted.load(), "Eight-source admission limit ignored");
    third->processBlock(buffer, midi); service.drain();
    require(third->captureSource()->summary.samples == 0, "Waiting source captured");
    const auto retained = second->captureSource();
    second.reset();
    require(registry.sources().size() == 8 && retained->id != 0 && third->captureSource()->admitted.load(), "Removal/admission invalidated ownership");
    extra.clear();
    service.reset();
    third->processBlock(buffer, midi); service.drain();
    require(third->captureSource()->summary.samples == 32, "Admitted unopened source did not capture");
    // Registry lifecycle updates from a host control thread while audio runs.
    service.reset();
    std::thread lifecycle([] { for (int n = 0; n < 20; ++n) { ScopeProcessor transient; } });
    for (int n = 0; n < 100; ++n) { first.processBlock(buffer, midi); playHead.advance(32); service.drain(); }
    lifecycle.join();
    require(first.captureSource()->summary.samples == 3200, "Concurrent source lifecycle interrupted capture");

    viewers.open(); viewers.close(); require(service.isCapturing(), "Second viewer close stopped capture");
    viewers.close(); require(! service.isCapturing() && first.captureSource()->queue.ready() == 0, "Last viewer left queued capture enabled");
    first.processBlock(buffer, midi); require(first.captureSource()->queue.ready() == 0, "Closed viewers still capture");
    viewers.open(); require(first.captureSource()->summary.samples == 0, "Reopening retained old session");
    first.processBlock(buffer, midi); service.drain();
    require(first.captureSource()->summary.samples == 32, "Reopened capture failed");
    viewers.close();
    {
        // Exercise the real editor lease in both diagnostic and no-probes builds.
        std::unique_ptr<juce::AudioProcessorEditor> editor(first.createEditorIfNeeded());
        require(editor != nullptr && service.isCapturing(), "Editor failed to start raw capture");
        first.processBlock(buffer, midi); third->processBlock(buffer, midi); service.drain();
        require(first.captureSource()->summary.samples == 32 && third->captureSource()->summary.samples == 32,
                "One real editor failed to capture the unopened source");
    }
    require(! service.isCapturing(), "Editor teardown failed to stop raw capture");
}
}
int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juce;
        queueTests(); processorLoopTransport(); serviceTests();
        require(SourceRegistry::instance().sources().empty(), "Capture registry leaked sources");
        std::cout << "PASS: raw stereo publication, chunk timestamps, callback identity/allocation, two-source 30 Hz capture, "
                     "50 ms pause, saturation/Reset, generations, lifecycle, and viewer leases\n";
        return 0;
    }
    catch (const std::exception& error)
    { countAllocations = false; std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
