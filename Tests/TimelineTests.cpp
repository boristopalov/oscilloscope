#include "PluginEditor.h"
#include <iostream>
#include <stdexcept>

namespace
{
using namespace oscilloscope;
void require(bool condition, const char* message) { if (! condition) throw std::runtime_error(message); }
struct Fixture
{
    DisplayService& service = DisplayService::instance();
    std::vector<std::shared_ptr<SourceState>> sources;
    std::vector<std::uint64_t> sequences;
    double now = juce::Time::getMillisecondCounterHiRes();
    explicit Fixture(int count)
    {
        for (int n = 0; n < count; ++n)
        { sources.push_back(SourceRegistry::instance().registerSource()); sources.back()->sampleRate.store(48000); sequences.push_back(0); }
        service.openViewer(sources[0]->id);
    }
    ~Fixture()
    {
        service.closeViewer();
        for (const auto& source : sources) SourceRegistry::instance().unregisterSource(source);
    }
    CaptureHeader header(int index, std::int64_t localSample, double ppq, int size = 128, double bpm = 120)
    {
        CaptureHeader h;
        h.generation = service.captureState.load() >> 1; h.sequence = ++sequences[static_cast<std::size_t>(index)];
        h.valid = hasPosition | hasSamples | hasPpq | hasBpm | hasMeter | hasBarStart;
        h.samplePosition = localSample; h.ppq = ppq; h.bpm = bpm; h.sampleCount = size;
        h.playing = true; h.numerator = 4; h.denominator = 4; h.barStartPpq = 4 * std::floor(ppq / 4);
        return h;
    }
    void push(int index, CaptureHeader h, int impulse = -1)
    {
        std::vector<float> left(static_cast<std::size_t>(h.sampleCount)), right(left.size());
        if (impulse >= 0) { left[static_cast<std::size_t>(impulse)] = 1.5f; right[static_cast<std::size_t>(impulse)] = -2.5f; }
        require(sources[static_cast<std::size_t>(index)]->queue.push(left.data(), right.data(), h.sampleCount, h), "Fixture queue unexpectedly full");
    }
    void tick(double advanceMs = 10) { now += advanceMs; service.drain(now); }
    void reset()
    {
        service.reset(); now = juce::Time::getMillisecondCounterHiRes();
    }
};
void boundedHistory()
{
    SourceHistory history;
    auto packet = std::make_unique<CapturePacket>(); packet->header.sampleCount = 2048;
    constexpr int blocks = 400;
    for (int block = 0; block < blocks; ++block)
    {
        for (int n = 0; n < 2048; ++n)
        { packet->left[static_cast<std::size_t>(n)] = static_cast<float>(block * 2048 + n); packet->right[static_cast<std::size_t>(n)] = -packet->left[static_cast<std::size_t>(n)]; }
        history.append(*packet, -1000 + block * 2048);
    }
    require(history.size() == SourceHistory::capacitySamples && history.endSample() == -1000 + blocks * 2048,
            "History did not bound/evict samples");
    require(history.allocatedBytes() == SourceHistory::capacitySamples * sizeof(StereoSample) + history.summaryBytes(), "Raw history allocation exceeded cap");
    for (auto sample = history.beginSample(); sample < history.endSample(); sample += 997)
        require(juce::exactlyEqual(history.at(sample).left, static_cast<float>(sample + 1000)) && juce::exactlyEqual(history.at(sample).right, -static_cast<float>(sample + 1000)),
                "Circular history coordinates or stereo samples corrupted");
    const auto bytes = history.allocatedBytes(); history.clear();
    require(history.size() == 0 && history.allocatedBytes() == bytes, "Reset grew or discarded bounded storage");
}
void presentationAndSuspendedPause()
{
    PresentationClock clock;
    clock.observe(48000, 1);
    double previous = clock.cursorAt(1);
    for (int frame = 1; frame <= 600; ++frame)
    {
        const double now = 1 + frame / 60.0;
        if (frame % 3 == 0) clock.observe(now * 48000, now + (frame % 2 ? 0.001 : -0.001));
        const auto current = clock.cursorAt(now);
        require(current > previous && std::abs((current - previous) / 800 - 1) < 0.01,
            "Capture observation stepped or stalled the presentation clock");
        previous = current;
    }
    Fixture f(2);
    for (int index = 0; index < 2; ++index)
    { auto h = f.header(index, index*1000000,0); h.captureTime=100; f.push(index,h,3); }
    f.tick(); ViewerModel viewer(f.service);
    const auto first = viewer.window(100.2);
    for (int index = 0; index < 2; ++index)
    { auto h = f.header(index,index*1000000+128,128.0/24000); h.captureTime=100+128.0/48000; h.playing=false; f.push(index,h); }
    f.tick();
    const auto generation = f.service.captureState.load() >> 1;
    f.tick(1000);
    require(! f.service.held() && viewer.window(101.2).end > first.end+1.9,
        "Suspended stopped callbacks froze the presentation clock");
    require(viewer.freeze(), "Could not freeze the displayed paused viewport");
    const auto frozen = viewer.window().end;
    for (int index = 0; index < 2; ++index)
    { auto h = f.header(index,index*9000000,0); h.captureTime=101.2; f.push(index,h); }
    f.tick();
    require(! f.service.held() && (f.service.captureState.load()>>1)==generation && juce::exactlyEqual(viewer.window(102).end, frozen),
        "Resume reset capture or changed a frozen viewer");
    const auto& history = f.sources[0]->history;
    require(history.hasSample(3) && ! history.hasSample(1000) && history.endSample()>57000,
        "Resumed history lost old samples or filled a suspended callback interval");
    viewer.unfreeze();
    require(viewer.window(101.3).end>first.end+2, "Unfreeze did not rejoin the common presentation clock");
    f.reset();
    auto independent = f.header(0,0,0); independent.valid &= ~hasPpq; independent.captureTime=200;
    f.push(0,independent,3); f.tick();
    ViewerModel fallback(f.service); fallback.window(200.2);
    require(fallback.freeze() && std::abs(fallback.lanes()[0].frozenEnd-3840)<1e-6,
        "Independent freeze jumped from the displayed delayed camera to the latest packet");
}
void eightSourceCadence()
{
    Fixture f(8);
    for (const int size : { 32, 64, 128, 256, 512, 1024, 2048 })
    {
        f.reset();
        int nextTick = 800;
        const int blocks = 48000 / size;
        for (int block = 0; block < blocks; ++block)
        {
            for (int n = 7; n >= 0; --n)
                f.push(n, f.header(n, 1008357449LL * n + block * size, block * size / 24000.0, size));
            if ((block + 1) * size >= nextTick) { f.tick(1000.0 / DisplayService::framesPerSecond); nextTick += 800; }
        }
        f.tick(); f.tick();
        for (const auto& source : f.sources)
            require(source->summary.samples == static_cast<std::uint64_t>(blocks * size)
                    && source->droppedCallbacks.load() == 0 && source->summary.status == CaptureStatus::live,
                    "Eight-source normal 60 Hz cadence lost or rejected data");
        f.reset();
        const int pausedBlocks = (2400 + size - 1) / size;
        for (int block = 0; block < pausedBlocks; ++block)
            for (int n = 0; n < 8; ++n)
                f.push(n, f.header(n, 1008357449LL * n + block * size, block * size / 24000.0, size));
        f.tick(50); f.tick();
        for (const auto& source : f.sources)
            require(source->summary.samples == static_cast<std::uint64_t>(pausedBlocks * size)
                    && source->summary.status == CaptureStatus::live,
                    "Eight-source 50 ms paused consumer lost data");
    }
}
void anchorsAndAlignment()
{
    for (double bpm : { 60.0, 120.0 })
    {
        Fixture f(8); ViewerModel view(f.service);
        for (int block = 0; block < 20; ++block)
        {
            for (int order = 0; order < 8; ++order)
            {
                const int source = block % 2 == 0 ? 7 - order : order;
                const auto sample = block * 128;
                // Eight independent sample-clock origins and sequence numbers.
                f.sequences[static_cast<std::size_t>(source)] += block == 0 ? static_cast<std::uint64_t>(source * 97) : 0;
                f.push(source, f.header(source, sample + 1008357449LL * source, 8 + sample * bpm / (60 * 48000), 128, bpm), 37);
            }
            f.tick(); view.window();
        }
        require(! f.service.held(), "Steady forward playback was held");
        double expectedBeat = 0;
        for (int n = 0; n < 8; ++n)
        {
            const auto& source = *f.sources[static_cast<std::size_t>(n)];
            require(source.summary.samples == 2560 && source.history.size() == 2560 && source.summary.status == CaptureStatus::live,
                    "Eight-source callback-order-independent ingestion failed");
            const auto beat = source.timeline.timing().beatAt(static_cast<double>(source.history.beginSample() + 37));
            if (n == 0) expectedBeat = beat;
            require(std::abs(beat - expectedBeat) * 60 * 48000 / bpm <= 1, "Equal-delay impulses misaligned across sample-clock origins");
            require(source.history.at(source.history.beginSample() + 37).left == 1.5f
                    && juce::exactlyEqual(source.history.at(source.history.beginSample() + 37).right, -2.5f), "History clamped or changed stereo audio");
        }
        const auto window = view.window();
        require(window.synced && std::abs(window.end - (8 + 2560 * bpm / (60 * 48000))) < 1e-10,
                "Freshness watermark used callback arrival order");
        require(juce::exactlyEqual(window.barOrigin, 8.0) && ! window.assumedBarOrigin && ! window.assumedMeter, "Host bar/meter not used");
        require(f.service.visualizationBytes() < DisplayService::visualizationBudgetBytes, "Eight live histories exceed budget");
    }
    Fixture f(2); ViewerModel view(f.service);
    f.push(0, f.header(0, 10000, 4, 2048), 17);
    f.push(1, f.header(1, 7770000, 4, 2048), 1041); f.tick();
    require(f.service.setOffset(f.sources[1]->id, -1024, OffsetUnit::samples), "Sample offset rejected");
    const auto firstBeat = f.sources[0]->timeline.timing().beatAt(10017);
    const auto corrected = f.sources[1]->timeline.timing().beatAt(7771041, f.sources[1]->offsetSamples());
    require(std::abs(firstBeat - corrected) * 24000 <= 1, "Known-delay negative correction did not align impulses");
    require(f.service.setOffset(f.sources[1]->id, -1024.0 / 48, OffsetUnit::milliseconds)
            && std::abs(f.sources[1]->offsetSamples() + 1024) < 1e-9, "Millisecond offset conversion failed");
    require(! f.service.setOffset(f.sources[1]->id, std::numeric_limits<double>::infinity(), OffsetUnit::samples), "Non-finite offset accepted");
    require(f.sources[1]->history.at(7771041).left == 1.5f, "Display correction altered stored samples");
    for (const auto choice : { BeatWindow::quarterBeat, BeatWindow::beat, BeatWindow::bar, BeatWindow::twoBars, BeatWindow::fourBars })
    {
        view.setWindow(choice); const auto window = view.window();
        require(std::abs(window.end - window.begin - ViewerModel::beatSpan(choice, 4)) < 1e-10, "Beat window span wrong");
    }
}
void loopContinuityAndResume()
{
    // Reproduce Live's split boundary: 439 samples to the end at 140 BPM,
    // then 73 samples at PPQ zero. The second tap changes its huge clock origin.
    Fixture f(2);
    const auto looped = [](CaptureHeader h)
    { h.looping = true; h.valid |= hasLoopBounds; h.loopStartPpq = 0; h.loopEndPpq = 32; return h; };
    f.push(0, looped(f.header(0, 657847, 31.978666666666669, 439, 140)), 17);
    f.push(1, looped(f.header(1, 1009015296, 31.978666666666669, 439, 140)), 17); f.tick();
    const auto firstSample = f.sources[0]->history.beginSample();
    f.push(0, looped(f.header(0, 0, 0, 73, 140)), 9);
    f.push(1, looped(f.header(1, 0, 0, 73, 140)), 9); f.tick();
    require(! f.service.held() && f.sources[0]->history.size() == 512 && f.sources[1]->history.size() == 512,
            "Measured Live loop wrap did not retain every sample");
    const auto& a = f.sources[0]->timeline.timing(); const auto& b = f.sources[1]->timeline.timing();
    require(std::abs(a.beatAt(static_cast<double>(firstSample + 439)) - 32) < 140.0 / (60 * 48000)
        && std::abs(a.beatAt(static_cast<double>(f.sources[0]->history.endSample())) - b.beatAt(static_cast<double>(f.sources[1]->history.endSample()))) < 1e-9,
        "Loop wrap changed stereo impulse alignment or display epoch");
    auto off0 = f.header(0, 73, 73.0 * 140 / (60 * 48000), 128, 140);
    auto off1 = f.header(1, 73, off0.ppq, 128, 140);
    f.push(0, off0); f.push(1, off1); f.tick();
    require(! f.service.held(), "Disabling loop after a wrap held capture");
    off0 = f.header(0, 201, 201.0 * 140 / (60 * 48000), 128, 140);
    off1 = f.header(1, 201, off0.ppq, 128, 140);
    f.push(0, looped(off0)); f.push(1, looped(off1)); f.tick();
    require(! f.service.held(), "Enabling loop during continuous playback held capture");
    f.reset(); require(! f.service.held(), "Manual Reset with loop enabled held capture");
    // Short, non-zero-start loop; a newly inserted source joins the current
    // pass using a common host timestamp, even after many cycles.
    TimelineMapper reference, joined;
    CaptureHeader h; h.valid = hasPosition | hasSamples | hasPpq | hasBpm | hasLoopBounds | hasHostTime;
    h.playing = h.looping = true; h.bpm = 120; h.loopStartPpq = 8; h.loopEndPpq = 8.01;
    h.ppq = 8; h.sampleCount = 240; h.hostTimeNs = 1000000000;
    for (int cycle = 0; cycle < 50; ++cycle)
    {
        h.hostTimeNs = 1000000000 + static_cast<std::uint64_t>(cycle) * 5000000;
        const auto result = reference.inspect(h);
        require(result == TimingResult::synced, "Repeated short/nonzero loops lost continuity");
        reference.accept(h, result);
    }
    auto late = h; late.samplePosition = 9000000;
    const auto lateStart = joined.accept(late, joined.inspect(late), reference.initialBeatNear(late));
    require(std::abs(joined.timing().beatAt(static_cast<double>(lateStart)) - 8.49) < 1e-9, "Late source joined the wrong loop epoch");
    auto seek = late; seek.ppq = 8.005;
    require(joined.inspect(seek) == TimingResult::changed, "Arbitrary seek inside active loop was accepted as a wrap");
    // Pause and Play keep one continuous history/generation; freeze is local.
    f.push(0, f.header(0, 5000, 1), 3); f.push(1, f.header(1, 20000, 1), 3); f.tick();
    ViewerModel frozen(f.service); require(frozen.freeze(), "Pause/resume freeze setup failed");
    const auto frozenEnd = frozen.window().end;
    const auto generation = f.service.captureState.load() >> 1;
    for (int index = 0; index < 2; ++index)
    { auto stopped = f.header(index, index == 0 ? 5128 : 20128, 1); stopped.playing = false; f.push(index, stopped); }
    f.tick(); require(! f.service.held(), "Pause still holds capture");
    f.push(0, f.header(0, 0, 0)); f.push(1, f.header(1, 30000, 0)); f.tick();
    require(! f.service.held() && (f.service.captureState.load() >> 1) == generation,
            "Play discarded history or changed generation");
    f.push(0, f.header(0, 128, 128.0 / 24000)); f.push(1, f.header(1, 30128, 128.0 / 24000)); f.tick();
    require(f.sources[0]->history.size() == 512 && f.sources[1]->history.size() == 512
            && frozen.frozen() && juce::exactlyEqual(frozen.window().end, frozenEnd),
            "Continuous resume lost history or changed frozen view");
    require(std::abs(f.sources[0]->timeline.timing().beatAt(static_cast<double>(f.sources[0]->history.endSample()))
        - f.sources[1]->timeline.timing().beatAt(static_cast<double>(f.sources[1]->history.endSample()))) < 1e-9,
        "Resume lost alignment across independent source clocks");

}
void shortLoopBacklog()
{
    Fixture f(3); f.service.selectReference(f.sources[2]->id); f.reset();
    for (int block = 0; block < 16; ++block)
        for (int index = 2; index >= 0; --index)
        {
            const int sample = (block * 128) % 512;
            auto h = f.header(index, sample + index * 1000000, sample / 24000.0);
            h.looping = true; h.valid |= hasLoopBounds; h.loopStartPpq = 0; h.loopEndPpq = 512.0 / 24000;
            f.push(index, h, 9);
        }
    f.tick();
    for (const auto& source : f.sources)
        require(! f.service.held() && source->history.size() == 2048
            && std::abs(source->timeline.timing().beatAt(static_cast<double>(source->history.endSample())) - 2048.0 / 24000) < 1e-9,
            "Short-loop FIFO backlog changed source epochs without host system timestamps");
}
void roundedLoopBoundaries()
{
    // A 16-beat loop at 130 BPM lasts 354461.538... samples. Live
    // rounds each pass to 354462. The old fixed anchor held at beat 48.
    Fixture f(2);
    constexpr double bpm = 130, loopBeats = 16;
    const double scale = 60 * supportedSampleRate / bpm;
    const auto loopSamples = static_cast<int>(std::ceil(loopBeats * scale));
    const std::array<std::int64_t, 2> origins { 0, 1008357449LL };
    std::int64_t retainedSamples = 0;
    for (int pass = 0; pass < 12; ++pass)
        for (int sample = 0; sample < loopSamples;)
        {
            const int count = std::min(2048, loopSamples - sample);
            for (int source = 0; source < 2; ++source)
            {
                auto h = f.header(source, (pass == 0 ? origins[static_cast<std::size_t>(source)] : 0) + sample,
                                  sample / scale, count, bpm);
                h.looping = true; h.valid |= hasLoopBounds;
                h.loopStartPpq = 0; h.loopEndPpq = loopBeats;
                f.push(source, h, 9);
            }
            f.tick(); sample += count; retainedSamples += count;
            require(! f.service.held(), "Rounded loop boundaries accumulated into a false timing hold");
            for (const auto& source : f.sources)
                require(source->summary.samples == static_cast<std::uint64_t>(retainedSamples),
                        "Loop rounding dropped or duplicated raw samples");
        }
    const auto& a = f.sources[0]->timeline.timing();
    const auto& b = f.sources[1]->timeline.timing();
    const double finalBeat = a.beatAt(static_cast<double>(f.sources[0]->history.endSample()));
    require(std::abs(finalBeat - 12 * loopBeats) * scale <= TimelineMapper::sampleTolerance
        && std::abs(finalBeat - b.beatAt(static_cast<double>(f.sources[1]->history.endSample()))) < 1e-9,
        "Rounded loops lost the shared beat epoch across independent clock origins");
    auto seek = f.header(0, 100, 4, 128, bpm);
    seek.looping = true; seek.valid |= hasLoopBounds; seek.loopEndPpq = loopBeats;
    f.push(0, seek); f.tick();
    require(f.service.holdReason() == HoldReason::timingChanged, "Seek after rounded loops escaped timing checks");
}
void holdsAndGenerations()
{
    for (const auto error : { TimingResult::changed, TimingResult::lost })
    {
        Fixture f(2); ViewerModel a(f.service), b(f.service); f.service.openViewer(f.sources[1]->id);
        f.push(0, f.header(0, 0, 0), 7); f.push(1, f.header(1, 50000, 0), 7); f.tick();
        a.window(); b.window(); require(a.freeze() && b.freeze(), "Two viewer freeze setup failed");
        const auto frozenA = a.window(); const auto oldGeneration = f.service.captureState.load() >> 1;
        auto changed = f.header(1, 50128, 128.0 / 24000);
        if (error == TimingResult::changed) changed.bpm = 123;
        if (error == TimingResult::lost) changed.valid &= ~hasPpq;
        f.push(1, changed); f.tick();
        require(f.service.held() && f.sources[1]->history.size() == 128, "Offending packet entered held history");
        ViewerModel openedWhileHeld(f.service);
        require(openedWhileHeld.window().synced && openedWhileHeld.window().seconds > 0,
                "Viewer opened during a hold could not display retained beat history");
        const auto retained = f.sources[0]->history.size();
        auto heldPacket = f.header(0, 128, 128.0 / 24000);
        f.push(0, heldPacket); f.tick();
        require(f.sources[0]->history.size() == retained, "Shared hold still appended another source");
        f.reset();

        require(! f.service.held() && f.sources[0]->history.size() == 0 && f.sources[1]->history.size() == 0,
                "Shared Reset did not clear histories and timing state");
        require(a.frozen() && b.frozen() && a.lanes()[0].at(7).left == 1.5f && juce::exactlyEqual(a.window().end, frozenA.end),
                "Shared Reset changed frozen samples/window/anchor");
        auto old = f.header(0, 9999, 999); old.generation = oldGeneration;
        f.push(0, old); f.sources[0]->lostGeneration.store(oldGeneration);
        f.push(0, f.header(0, 1000000, 8), 9); f.push(1, f.header(1, 9, 8), 9); f.tick();
        require(! f.service.held() && f.sources[0]->history.size() == 128 && f.sources[0]->timeline.timing().sample == 1000000,
                "Old-generation packet or loss contaminated new anchor/history");
        require(a.lanes()[0].at(7).left == 1.5f, "Fresh live ingestion changed frozen snapshot");
        f.service.closeViewer();
    }
    for (const auto seek : { -100000LL, 100000LL })
    {
        Fixture f(1); f.push(0, f.header(0, 200000, 8)); f.tick();
        f.push(0, f.header(0, 200128 + seek, (200128.0 + static_cast<double>(seek)) / 24000)); f.tick();
        require(f.service.holdReason() == HoldReason::timingChanged && f.sources[0]->history.size() == 128, "Forward/backward seek missed");
    }
    // Fixed anchor must also catch accumulating drift, not just pairwise errors.
    Fixture drift(1); drift.push(0, drift.header(0, 0, 0)); drift.tick();
    for (int n = 1; n <= 4; ++n)
    { drift.push(0, drift.header(0, n * 128, (n * 128 + n * 0.4) / 24000)); drift.tick(); }
    require(drift.service.held(), "Accumulating anchor drift escaped pairwise continuity checks");
    drift.reset(); drift.push(0, drift.header(0, 0, 0)); drift.tick();
    for (int n = 1; n <= 3; ++n)
    { drift.push(0, drift.header(0, n * 129, n * 129 / 24000.0)); drift.tick(); }
    require(drift.service.held(), "Repeated tolerated sample-clock errors drifted from raw storage coordinates");
}
void lossFreshnessAndReference()
{
    Fixture f(3); ViewerModel view(f.service);
    for (int n = 0; n < 3; ++n) f.push(n, f.header(n, n * 10000, 0), 2);
    f.tick(); view.window();
    f.sources[1]->lostGeneration.store(f.service.captureState.load() >> 1); f.tick();
    require(! f.service.held() && f.sources[1]->summary.status == CaptureStatus::captureGap,
            "Non-reference loss held healthy sources or was invisible without a packet");
    f.push(0, f.header(0, 128, 128.0 / 24000)); f.tick(510);
    require(! f.service.held() && f.sources[2]->summary.status == CaptureStatus::stale, "Stalled non-reference source froze reference");
    const auto end = view.window().end;
    require(std::abs(end - 256.0 / 24000) < 1e-10, "Stale/gapped source constrained watermark");
    f.push(2, f.header(2, 20128, 128.0 / 24000)); f.tick();
    require(f.sources[2]->summary.status == CaptureStatus::stale && f.sources[2]->history.size() == 128, "Stale source automatically rejoined");
    require(f.service.selectReference(f.sources[2]->id) && f.service.held(), "Reference change did not require Reset");
    f.reset();
    for (int n = 0; n < 3; ++n) f.push(n, f.header(n, n * 10000, 4));
    f.tick(); require(! f.service.held(), "Reset failed stale/gap/reference recovery");
    f.sources[2]->lostGeneration.store(f.service.captureState.load() >> 1); f.tick();
    require(f.service.holdReason() == HoldReason::captureGap, "Reference loss did not hold shared session");
    f.reset(); f.push(2, f.header(2, 999, 4)); f.tick(); f.tick(510);
    require(f.service.holdReason() == HoldReason::referenceStale && f.service.referenceId() == f.sources[2]->id,
            "Stalled reference automatically replaced or failed to hold");
    f.reset(); const auto referenceId = f.sources[2]->id;
    SourceRegistry::instance().unregisterSource(f.sources[2]); f.tick();
    require(f.service.holdReason() == HoldReason::referenceMissing && f.service.referenceId() == referenceId,
            "Removed reference was replaced automatically");
}
void lateAdmissionAndOwnership()
{
    Fixture f(1); ViewerModel view(f.service);
    f.push(0, f.header(0, 0, 0)); f.tick();
    f.push(0, f.header(0, 128, 128.0 / 24000)); f.tick(1000);
    const auto added = SourceRegistry::instance().registerSource(); added->sampleRate.store(48000);
    f.sources.push_back(added); f.sequences.push_back(0);
    f.push(0, f.header(0, 256, 256.0 / 24000)); f.tick();
    require(added->summary.status == CaptureStatus::waiting, "New tap was immediately stale before its first callback");
    f.push(0, f.header(0, 384, 384.0 / 24000));
    f.push(1, f.header(1, 99999999, 384.0 / 24000), 3); f.tick();
    require(! f.service.held() && added->summary.status == CaptureStatus::live, "Late tap could not join independent clock anchor");
    const auto lanes = view.lanes();
    require(view.freeze(), "Late tap freeze setup failed");
    SourceRegistry::instance().unregisterSource(added); f.tick();
    require(lanes[1].at(lanes[1].begin + 3).left == 1.5f && view.lanes()[1].at(view.lanes()[1].begin + 3).left == 1.5f,
            "Source removal invalidated a borrowed live lane or independent freeze copy");
}
void unavailableTimingAndCompatibility()
{
    Fixture f(3); ViewerModel view(f.service);
    auto fallback = f.header(1, 999, 0); fallback.valid &= ~hasPpq;
    auto conflict = f.header(2, 123, 0); conflict.bpm = 123;
    f.push(0, f.header(0, 0, 0)); f.push(1, fallback); f.push(2, conflict); f.tick();
    require(! f.service.held() && f.sources[1]->summary.status == CaptureStatus::syncUnavailable
            && f.sources[2]->summary.status == CaptureStatus::incompatible, "Initial timing fallback/conflict handling wrong");
    require(view.window().synced && ! view.lanes()[1].synced && ! view.lanes()[2].synced, "Fallback/conflict implied synchronized comparison");
    f.push(1, f.header(1, 1127, 128.0 / 24000)); f.tick();
    require(f.sources[1]->summary.status == CaptureStatus::syncUnavailable, "Fallback invented a mid-session beat anchor");
    f.reset();
    auto assumed = f.header(0, 0, 0); assumed.valid &= ~(hasMeter | hasBarStart);
    f.push(0, assumed); f.tick();
    require(view.window().assumedBarOrigin && view.window().assumedMeter, "Unavailable bar origin/meter not labeled");
    f.sources[0]->sampleRate.store(44100); f.sources[0]->unsupportedRateRevision.fetch_add(1); f.tick();
    require(f.service.held() && f.sources[0]->history.size() == 0, "Leaving 48 kHz retained live history");
    f.sources[0]->sampleRate.store(48000); f.push(0, f.header(0, 128, 128.0 / 24000)); f.tick();
    require(f.service.held() && f.sources[0]->history.size() == 0, "Returning to 48 kHz resumed without Reset");
    f.reset();
    // Unsupported rate entered and left BETWEEN display ticks must be latched too.
    f.push(0, f.header(0, 0, 0)); f.tick(); f.sources[0]->unsupportedRateRevision.fetch_add(1);
    f.push(0, f.header(0, 128, 128.0 / 24000)); f.tick();
    require(f.service.held() && f.sources[0]->history.size() == 0, "Between-tick sample-rate transition lost");
}
void windowsAndBudget()
{
    Fixture f(8); ViewerModel a(f.service), b(f.service);
    for (int block = 0; block < 376; ++block)
    {
        for (int n = 0; n < 8; ++n) f.push(n, f.header(n, block * 2048 + n * 1000000LL,
            block * 2048.0 / 96000, 2048, 30), block == 375 ? 1 : -1);
        f.tick(2048.0 / 48);
    }
    a.setWindow(BeatWindow::fourBars); b.setWindow(BeatWindow::beat);
    const auto window = a.window();
    require(window.historyLimited && juce::exactlyEqual(window.seconds, 32.0) && f.sources[0]->history.size() == SourceHistory::capacitySamples,
            "Slow-tempo four-bar window silently grew history");
    b.setVisible(f.sources[1]->id, false);
    require(a.lanes().size() == 8 && b.lanes().size() == 7 && ! juce::exactlyEqual(a.window().seconds, b.window().seconds),
            "Per-view source choices or beat windows leaked across viewers");
    const auto baseline = f.service.visualizationBytes();
    require(a.freeze() && b.freeze(), "Two bounded visible-region freezes failed");
    const auto frozenBytes = f.service.visualizationBytes();
    require(frozenBytes > baseline && frozenBytes < DisplayService::visualizationBudgetBytes, "Freeze allocations not budgeted");
    require(! f.service.reserveSnapshot(DisplayService::visualizationBudgetBytes), "Oversized snapshot reservation accepted");
    const auto frozenAnchor = a.lanes()[0].anchor;
    f.reset(); f.push(0, f.header(0, 999999, 16)); f.tick();
    require(a.frozen() && a.lanes()[0].anchor.sample == frozenAnchor.sample && a.lanes()[0].end - a.lanes()[0].begin <= 768000,
            "Reset changed frozen anchor or freeze exceeded visible raw bound");
    a.unfreeze(); b.unfreeze(); require(f.service.visualizationBytes() == baseline, "Freeze memory reservation leaked");
}
void actualEditors()
{
    class PlayHead final : public juce::AudioPlayHead
    {
    public:
        juce::Optional<PositionInfo> getPosition() const override { return position; }
        PositionInfo position;
    } playHead;
    playHead.position.setIsPlaying(true); playHead.position.setBpm(120);
    playHead.position.setTimeSignature(juce::AudioPlayHead::TimeSignature { 4, 4 });
    playHead.position.setPpqPositionOfLastBarStart(0);
    ScopeProcessor a, b; a.prepareToPlay(48000, 512); b.prepareToPlay(48000, 512);
    a.setPlayHead(&playHead); b.setPlayHead(&playHead);
    std::unique_ptr<juce::AudioProcessorEditor> first(a.createEditorIfNeeded()), second(b.createEditorIfNeeded());
    auto& service = a.displayService();
    require(service.referenceId() == a.captureSource()->id, "Second editor changed shared reference");
    juce::AudioBuffer<float> audio(2, 512); juce::MidiBuffer midi;
    for (int block = 0; block < 282; ++block)
    {
        playHead.position.setTimeInSamples(block * 512); playHead.position.setPpqPosition(block * 512 / 24000.0);
        for (int n = 0; n < 512; ++n)
        {
            const auto position = block * 512 + n;
            const auto phase = static_cast<double>(position % 24000);
            const auto sample = static_cast<float>(std::sin(phase * 0.02) * std::exp(-phase / 3000) * 1.5);
            audio.setSample(0, n, sample); audio.setSample(1, n, sample * -0.75f);
        }
        a.processBlock(audio, midi); b.processBlock(audio, midi); service.drain();
    }
    // Exercise paint, controls and snapshots through the actual JUCE component API.
    auto click = [](juce::Component& parent, const juce::String& text)
    {
        for (int n = 0; n < parent.getNumChildComponents(); ++n)
            if (auto* button = dynamic_cast<juce::Button*>(parent.getChildComponent(n)); button && button->getButtonText() == text)
            { if (button->onClick) button->onClick(); return; }
        throw std::runtime_error("Expected editor control missing");
    };
    for (int n = 0; n < first->getNumChildComponents(); ++n)
        if (auto* button = dynamic_cast<juce::Button*>(first->getChildComponent(n)))
            require(button->getButtonText() != "Reference info", "Redundant reference info control remains");
    const auto previewPath = juce::SystemStats::getEnvironmentVariable("OSCILLOSCOPE_TEST_PREVIEW", {});
    if (previewPath.isNotEmpty())
    {
        juce::FileOutputStream output { juce::File(previewPath) };
        require(output.openedOk(), "Could not open requested editor preview output");
        require(output.setPosition(0) && output.truncate().wasOk(), "Could not truncate editor preview output");
        juce::PNGImageFormat png;
        require(png.writeImageToStream(first->createComponentSnapshot(first->getLocalBounds()), output), "Could not write editor preview PNG");
    }
    click(*first, "Freeze");
    const auto generation = service.captureState.load() >> 1;
    click(*second, "Reset capture");
    require((service.captureState.load() >> 1) == generation + 1 && a.captureSource()->history.size() == 0,
            "Reset from second editor failed shared generation/history reset");
    first->createComponentSnapshot(first->getLocalBounds()); second->createComponentSnapshot(second->getLocalBounds());
    a.editorBeingDeleted(first.get()); first.reset(); require(service.isCapturing() && service.referenceId() == a.captureSource()->id,
                           "Closing reference editor changed reference or stopped shared ingestion");
    b.editorBeingDeleted(second.get()); second.reset(); require(! service.isCapturing(), "Last real viewer left capture enabled");
}
}
int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juce;
        boundedHistory(); presentationAndSuspendedPause(); eightSourceCadence(); anchorsAndAlignment(); loopContinuityAndResume(); shortLoopBacklog(); roundedLoopBoundaries(); holdsAndGenerations(); lossFreshnessAndReference(); lateAdmissionAndOwnership();
        unavailableTimingAndCompatibility(); windowsAndBudget(); actualEditors();
        require(SourceRegistry::instance().sources().empty(), "Timing tests leaked sources");
        std::cout << "PASS: bounded stereo history; eight independent clock origins and callback orders; impulse offsets; "
                     "shared holds/Reset and generations; stale/gap/reference exclusion; time fallback; rate transitions; "
                     "beat windows; two-view freezes/budget; actual editor controls/paint\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
