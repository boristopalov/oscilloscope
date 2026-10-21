#include "PluginEditor.h"
#include <iostream>
#include <stdexcept>

namespace
{
using namespace oscilloscope;
void require(bool condition, const char* message) { if (! condition) throw std::runtime_error(message); }
std::vector<juce::Component*> descendants(juce::Component& parent)
{
    std::vector<juce::Component*> result;
    for (int n = 0; n < parent.getNumChildComponents(); ++n)
    {
        auto* child = parent.getChildComponent(n); result.push_back(child);
        const auto nested = descendants(*child); result.insert(result.end(), nested.begin(), nested.end());
    }
    return result;
}
ViewLane samples(const std::vector<StereoSample>& data)
{
    ViewLane lane; lane.begin = 0; lane.end = static_cast<std::int64_t>(data.size()); lane.snapshot = data.data(); return lane;
}
void projectionsAndPeaks()
{
    const std::vector<StereoSample> identical { { 1.5f, 1.5f }, { -1.5f, -1.5f } };
    const std::vector<StereoSample> opposite { { 1.5f, -1.5f }, { -1.5f, 1.5f } };
    const auto side = WaveformRenderer::analyse(samples(identical), 0, 2, 1, ChannelMode::side);
    const auto mid = WaveformRenderer::analyse(samples(opposite), 0, 2, 1, ChannelMode::mid);
    require(juce::exactlyEqual(side.peak(), 0.0f) && juce::exactlyEqual(mid.peak(), 0.0f), "Identical stereo did not cancel S or opposite polarity did not cancel M");
    require(juce::exactlyEqual(side.originalPeak, 1.5f) && juce::exactlyEqual(mid.originalPeak, 1.5f), "Transformed cancellation hid original stereo over");
    const std::vector<StereoSample> asynchronous { { 2, 0 }, { 0, 2 }, { -2, 0 }, { 0, -2 } };
    auto frame = WaveformRenderer::analyse(samples(asynchronous), 0, 4, 1, ChannelMode::midSide);
    require(juce::exactlyEqual(frame.buckets[0].low[0], -1.0f) && juce::exactlyEqual(frame.buckets[0].high[0], 1.0f)
            && juce::exactlyEqual(frame.buckets[0].low[1], -1.0f) && juce::exactlyEqual(frame.buckets[0].high[1], 1.0f),
            "M/S extrema combined independent L/R extrema instead of projecting each sample");
    const std::array<std::array<float, 2>, 6> expected {{{ 2, 0 }, { -4, 0 }, { 2, -4 }, { -1, 0 }, { 3, 0 }, { -1, 3 }}};
    for (int mode = 0; mode < 6; ++mode)
        require(ChannelProjector::project({ 2, -4 }, static_cast<ChannelMode>(mode)) == expected[static_cast<std::size_t>(mode)], "Channel projection incorrect");
    frame = WaveformRenderer::analyse(samples(identical), 0, 2, 1, ChannelMode::leftRight);
    require(juce::exactlyEqual(frame.buckets[0].low[0], -1.5f) && juce::exactlyEqual(frame.buckets[0].high[0], 1.5f), "Signed over-full-scale extrema clamped");
    require(std::abs(WaveformRenderer::peakDb(frame.peak()) - 3.521825181) < 1e-8
            && WaveformRenderer::peakText(frame.peak()) == "+3.52 dBFS", "1.5 sample peak did not read +3.52 dBFS");
    require(std::isinf(WaveformRenderer::peakDb(0)) && WaveformRenderer::peakDb(0) < 0, "Silence did not read negative infinity");
    for (const auto headroomDb : { 6.0, 7.3, 12.0, 24.0 })
    {
        const auto scale = WaveformRenderer::amplitudeRange(headroomDb);
        const juce::Rectangle<float> area(0, 0, 100, 100);
        require(WaveformRenderer::yFor(1.5f, area, scale) < WaveformRenderer::yFor(1, area, scale)
                && WaveformRenderer::yFor(-1.5f, area, scale) > WaveformRenderer::yFor(-1, area, scale), "Over did not extend past both full-scale lines");
        require(juce::exactlyEqual(frame.peak(), 1.5f), "Display scaling changed sample peak");
    }
    require(juce::exactlyEqual(WaveformRenderer::amplitudeRange(0), 1.0f)
            && std::abs(WaveformRenderer::amplitudeRange(7.3) - 2.31739465f) < 1e-6f
            && std::abs(WaveformRenderer::amplitudeRange(24) - 15.84893192f) < 1e-5f,
            "Manual headroom did not convert dB to an absolute amplitude range");
}
void peakPreservationAndCoordinates()
{
    std::vector<StereoSample> data(SourceHistory::capacitySamples);
    for (const auto index : { 0, 1, 767, 123456, 383999, 767999 })
    {
        data[static_cast<std::size_t>(index)] = { 1.5f, -1.5f };
        for (const int width : { 1, 7, 997, 1600 })
        {
            auto frame = WaveformRenderer::analyse(samples(data), 0, 768000, width, ChannelMode::leftRight);
            require(juce::exactlyEqual(frame.peak(), 1.5f), "One-sample over disappeared at full 16-second zoom");
            bool positive = false, negative = false;
            for (const auto& bucket : frame.buckets) { positive |= juce::exactlyEqual(bucket.high[0], 1.5f); negative |= juce::exactlyEqual(bucket.low[1], -1.5f); }
            require(positive && negative, "One-sample bipolar over lost in pixel buckets");
        }
        data[static_cast<std::size_t>(index)] = {};
    }
    data.resize(4); data = { { 1, 0 }, { 2, 0 }, { 3, 0 }, { 4, 0 } };
    auto frame = WaveformRenderer::analyse(samples(data), 0.5, 3.5, 12, ChannelMode::left);
    require(frame.individualSamples && frame.points.size() == 3 && juce::exactlyEqual(frame.points[0].values[0], 2.0f)
            && juce::exactlyEqual(frame.points[2].values[0], 4.0f), "Close zoom did not expose individual samples at fractional window boundaries");
    require(std::abs(frame.points[0].x - 2) < 1e-6f && std::abs(frame.points[2].x - 10) < 1e-6f, "Sample trace x coordinates incorrect");
    frame = WaveformRenderer::analyse(samples(data), -4, 4, 8, ChannelMode::left);
    require(! frame.buckets[0].present && frame.buckets[4].present, "Missing history rendered as captured silence");
    frame = WaveformRenderer::analyse(samples(data), 0, 4, 4, ChannelMode::left);
    require(juce::exactlyEqual(frame.buckets[0].high[0], 1.0f) && juce::exactlyEqual(frame.buckets[1].low[0], 2.0f), "Half-open pixel buckets duplicated boundary samples");
    // Circular history and snapshots use the same renderer coordinates.
    SourceHistory history; auto packet = std::make_unique<CapturePacket>(); packet->header.sampleCount = 2048;
    for (int block = 0; block < 376; ++block) history.append(*packet, 800000 + block * 2048LL);
    packet->left[2047] = 1.5f; history.append(*packet, history.endSample());
    ViewLane lane; lane.history = &history; lane.begin = history.beginSample(); lane.end = history.endSample();
    frame = WaveformRenderer::analyse(lane, static_cast<double>(lane.begin), static_cast<double>(lane.end), 997, ChannelMode::left);
    require(juce::exactlyEqual(frame.peak(), 1.5f), "Wrapped history lost newest one-sample over");
}
void highlightAndOverflow()
{
    const std::vector<StereoSample> data { { -1.5f, 0 }, { 1.5f, 0 } };
    const auto frame = WaveformRenderer::analyse(samples(data), 0, 2, 10, ChannelMode::left);
    juce::Image image(juce::Image::ARGB, 20, 100, true); juce::Graphics graphics(image);
    const juce::Rectangle<float> area(2, 0, 10, 100);
    WaveformRenderer::draw(graphics, area, frame, juce::Colours::cyan, 2);
    int redOutside = 0, redInside = 0;
    for (int y = 0; y < 100; ++y) for (int x = 0; x < 20; ++x)
    {
        const auto colour = image.getPixelAt(x, y);
        if (colour.getAlpha() > 128 && colour.getRed() > 200 && colour.getGreen() < 160)
        { if (y < 24 || y > 76) ++redOutside; if (y > 27 && y < 73) ++redInside; }
    }
    require(redOutside > 0 && redInside == 0, "Over highlight extended into sub-full-scale portion of sample trace");
    const std::vector<StereoSample> extremes { { -8, 0 }, { 8, 0 } };
    const auto overflow = WaveformRenderer::analyse(samples(extremes), 0, 2, 10, ChannelMode::left);
    juce::Image fixed(juce::Image::ARGB, 20, 100, true); juce::Graphics fixedGraphics(fixed);
    WaveformRenderer::draw(fixedGraphics, area, overflow, juce::Colours::cyan, 2);
    require(fixed.getPixelAt(7, 1).getRed() > 200 && fixed.getPixelAt(2, 98).getRed() > 200, "Fixed range did not mark positive and negative edge overflow");
    std::vector<StereoSample> narrowOvers(10);
    narrowOvers[6].right = 1.5f; narrowOvers[7].right = -1.5f;
    const auto wide = WaveformRenderer::analyse(samples(narrowOvers), 0, 10, 5, ChannelMode::leftRight);
    juce::Image wideImage(juce::Image::ARGB, 10, 100, true); juce::Graphics wideGraphics(wideImage);
    WaveformRenderer::draw(wideGraphics, { 2, 0, 5, 100 }, wide, juce::Colours::cyan, 2);
    require(wideImage.getPixelAt(5, 15).getRed() > 200 && wideImage.getPixelAt(5, 85).getRed() > 200
            && wideImage.getPixelAt(5, 50).getRed() < 200,
            "Wide min/max drawing lost overs in a paired trace or highlighted below full scale");
    const std::vector<StereoSample> atFullScale { { 1, 1 }, { -1, -1 } };
    const auto boundary = WaveformRenderer::analyse(samples(atFullScale), 0, 2, 1, ChannelMode::leftRight);
    juce::Image boundaryImage(juce::Image::ARGB, 1, 100, true); juce::Graphics boundaryGraphics(boundaryImage);
    WaveformRenderer::draw(boundaryGraphics, { 0, 0, 1, 100 }, boundary, juce::Colours::cyan, 2);
    for (int y = 0; y < 100; ++y)
        require(boundaryImage.getPixelAt(0, y).getRed() < 200, "Exactly full scale was marked as a strict over");
}
void cachedExtrema()
{
    SourceHistory history; auto packet = std::make_unique<CapturePacket>(); packet->header.sampleCount = 2048;
    for (int block = 0; block < 377; ++block)
    {
        for (int n = 0; n < 2048; ++n)
        {
            const int index = block * 2048 + n;
            packet->left[static_cast<std::size_t>(n)] = static_cast<float>((index * 17) % 101 - 50) / 16;
            packet->right[static_cast<std::size_t>(n)] = static_cast<float>((index * 31) % 103 - 51) / 16;
        }
        history.append(*packet, -300000 + block * 2048LL);
    }
    auto compare = [&]
    {
        for (int offset : { 0, 1, 31, 63, 64, 121 })
            for (int span : { 1, 63, 64, 65, 129, 2048 })
            {
                const auto begin = history.beginSample() + offset, end = std::min(begin + span, history.endSample());
                SampleExtrema direct; for (auto sample = begin; sample < end; ++sample) direct.add(history.at(sample));
                const auto cached = history.extrema(begin, end);
                require(cached.present == direct.present && cached.low == direct.low && cached.high == direct.high,
                        "Cached extrema differed from direct L/R/M/S scan at circular/partial boundaries");
            }
        std::vector<StereoSample> raw(history.size());
        for (std::size_t n = 0; n < raw.size(); ++n) raw[n] = history.at(history.beginSample() + static_cast<std::int64_t>(n));
        ViewLane live; live.begin = history.beginSample(); live.end = history.endSample(); live.history = &history;
        ViewLane copied = live; copied.history = nullptr; copied.snapshot = raw.data();
        for (int mode = 0; mode < 6; ++mode)
        {
            const auto cached = WaveformRenderer::analyse(live, static_cast<double>(live.begin) + 0.25, static_cast<double>(live.end) - 0.25, 997, static_cast<ChannelMode>(mode));
            const auto direct = WaveformRenderer::analyse(copied, static_cast<double>(live.begin) + 0.25, static_cast<double>(live.end) - 0.25, 997, static_cast<ChannelMode>(mode));
            require(cached.peaks == direct.peaks && juce::exactlyEqual(cached.originalPeak, direct.originalPeak), "Cache changed sample peak readouts");
            for (std::size_t n = 0; n < cached.buckets.size(); ++n)
                require(cached.buckets[n].present == direct.buckets[n].present && cached.buckets[n].low == direct.buckets[n].low
                        && cached.buckets[n].high == direct.buckets[n].high, "Cache changed projected pixel extrema");
        }
    };
    compare(); history.clear(); packet->header.sampleCount = 17;
    for (int n = 0; n < 12; ++n) history.append(*packet, 900000 + n * 17LL);
    compare();
    for (int columns : { 5, 1800, 2048 }) history.bucketExtrema(900000, 900204, columns);
    require(history.allocatedBytes() <= SourceHistory::capacitySamples * sizeof(StereoSample) + history.summaryBytes()
            + SourceHistory::maxCachedColumns * sizeof(SampleExtrema), "Viewport cache grew beyond its fixed memory reservation");
    history.clear(); packet->header.sampleCount = 32;
    packet->left.fill(1); packet->right.fill(0); history.append(*packet, 0);
    require(juce::exactlyEqual(history.bucketExtrema(0, 32, 1)[0].high[0], 1.0f), "Viewport reset fixture failed");
    history.clear(); packet->left.fill(2); history.append(*packet, 0);
    require(juce::exactlyEqual(history.bucketExtrema(0, 32, 1)[0].high[0], 2.0f), "Reset reused cached extrema at identical sample coordinates");
}
void thresholdsAndOverlay()
{
    ScopeProcessor processor; const auto source = processor.captureSource();
    auto& service = processor.displayService();
    require(service.setClipThreshold(source->id, -6.020599913279624), "Valid threshold rejected");
    ViewerModel first(service), second(service);
    require(std::abs(first.lanes()[0].clipAmplitude() - 0.5f) < 1e-6f
        && juce::exactlyEqual(first.lanes()[0].clipThresholdDb, second.lanes()[0].clipThresholdDb), "Threshold not shared per track");
    require(! service.setClipThreshold(source->id, std::numeric_limits<double>::quiet_NaN())
        && ! service.setClipThreshold(source->id, 25) && ! service.setClipThreshold(source->id, -61),
        "Invalid threshold accepted");
    WaveformFrame frame; frame.mode = ChannelMode::left;
    frame.buckets = { { { -0.75f, 0 }, { 0.75f, 0 }, true } };
    juce::Image image(juce::Image::ARGB, 1, 100, true, juce::SoftwareImageType());
    WaveformRenderer::rasterize(image, frame, juce::Colours::cyan, 2, 0.5f);
    require(image.getPixelAt(0, 33).getRed() > 200 && image.getPixelAt(0, 50).getRed() < 100,
        "Custom threshold failed to limit highlight to excursions");
    image.clear(image.getBounds());
    frame.buckets[0].high[0] = 2.5f;
    WaveformRenderer::rasterize(image, frame, juce::Colours::cyan, 2, 3);
    require(image.getPixelAt(0, 0).getRed() < 100, "Visible-range overflow falsely triggered the raised clip threshold");
    frame.buckets[0].high[0] = 0.75f;
    image.clear(image.getBounds());
    WaveformRenderer::rasterize(image, frame, juce::Colours::cyan, 2, 2);
    require(image.getPixelAt(0, 33).getRed() < 100, "Raised threshold still highlighted below-threshold samples");
    juce::Image overlay(juce::Image::ARGB, 1, 100, true, juce::SoftwareImageType());
    WaveformRenderer::rasterize(overlay, frame, juce::Colours::cyan, 2, 1, true);
    const auto firstPixel = overlay.getPixelAt(0, 31);
    WaveformRenderer::rasterize(overlay, frame, juce::Colours::magenta, 2, 1, true);
    const auto mixed = overlay.getPixelAt(0, 31);
    require(firstPixel.getGreen() > 0 && mixed.getGreen() > 0 && mixed.getRed() > firstPixel.getRed()
        && mixed.getAlpha() < 255, "Overlay occluded the first source or lost transparency");
    require(overlay.getPixelAt(0, 50).getAlpha() == 0, "Overlay envelopes obscure other source contours");

    WaveformFrame ramp; ramp.mode = ChannelMode::left;
    for (float value : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
        ramp.buckets.push_back({ { value, 0 }, { value, 0 }, true });
    juce::Image connected(juce::Image::ARGB, 5, 100, true, juce::SoftwareImageType());
    WaveformRenderer::rasterize(connected, ramp, juce::Colours::cyan, 2, 1, true);
    require(connected.getPixelAt(1, 57).getAlpha() > 128
        && connected.getPixelAt(1, 66).getAlpha() > 128,
        "Overlay extrema remain isolated dots instead of connected thick lines");
    ramp.buckets[2].present = false;
    connected.clear(connected.getBounds());
    WaveformRenderer::rasterize(connected, ramp, juce::Colours::cyan, 2, 1, true);
    require(connected.getPixelAt(2, 50).getAlpha() == 0 && connected.getPixelAt(1, 57).getAlpha() == 0,
        "Waveform lines bridge unavailable history");
    WaveformFrame flat; flat.mode = ChannelMode::left;
    flat.buckets.resize(20, { {}, {}, true });
    juce::Image retina(juce::Image::ARGB, 20, 200, true, juce::SoftwareImageType());
    WaveformRenderer::rasterize(retina, flat, juce::Colours::cyan, 2, 1, true, 2);
    require(retina.getPixelAt(10, 97).getAlpha() > 128 && retina.getPixelAt(10, 102).getAlpha() > 128,
        "Retina contours lost their logical stroke thickness");
    const std::vector<StereoSample> pulse { { 0, 0 }, { 1.5f, 0 }, { 0, 0 } };
    const auto close = WaveformRenderer::analyse(samples(pulse), 0, 3, 12, ChannelMode::left);
    juce::Image closeRetina(juce::Image::ARGB, 12, 100, true, juce::SoftwareImageType());
    juce::Graphics closeGraphics(closeRetina); closeGraphics.addTransform(juce::AffineTransform::scale(2));
    WaveformRenderer::draw(closeGraphics, { 0, 0, 6, 50 }, close, juce::Colours::cyan, 2, 2, 2);
    require(closeRetina.getPixelAt(4, 13).getAlpha() > 128,
        "Retina sample lines moved away from their actual sample coordinates");
}
// A moving feature must not brighten/dim as it crosses former dash positions.
void stableTraceAndGrid()
{
    for (float scale : { 1.0f, 2.0f })
    {
        WaveformFrame frame; frame.mode = ChannelMode::leftRight;
        frame.buckets.resize(96, { { -0.5f, 0.5f }, { -0.5f, 0.5f }, true });
        juce::Image image(juce::Image::ARGB, 96, 100, true, juce::SoftwareImageType());
        WaveformRenderer::rasterize(image, frame, juce::Colours::cyan, 2, 1, true, scale);
        const auto ink = image.getPixelAt(0, 37);
        require(ink.getAlpha() > 128, "Second channel is too faint");
        for (int x = 1; x < image.getWidth(); ++x)
            require(image.getPixelAt(x, 37) == ink, "Second trace opacity depends on screen position");
    }
    auto grid = [](ViewWindow window)
    {
        juce::Image image(juce::Image::ARGB, 161, 100, true, juce::SoftwareImageType());
        juce::Graphics g(image);
        WaveformRenderer::drawGrid(g, { 0, 0, 160, 100 }, window, true, 2);
        return image;
    };
    ViewWindow window; window.begin = 0; window.end = 16; window.beatsPerBar = 4;
    const auto original = grid(window);
    window.begin += 0.37; window.end += 0.37; window.barOrigin = 0.19;
    const auto shifted = grid(window);
    for (int x = 0; x < 160; ++x)
        require(original.getPixelAt(x, 10) == shifted.getPixelAt(x, 10), "Grid moves with host phase");
    const auto background = original.getPixelAt(1, 10);
    require(original.getPixelAt(40, 10) != background && original.getPixelAt(80, 10) != background,
        "Bar duration markers missing");
    require(original.getPixelAt(10, 10) == background && original.getPixelAt(20, 10) == background,
        "Grid still contains beat subdivisions");
    window.begin = 0; window.end = 1;
    const auto shortWindow = grid(window);
    for (int x = 0; x < 160; ++x)
        require(shortWindow.getPixelAt(x, 10) == background, "Sub-bar window contains subdivisions");
    window.end = 12; window.beatsPerBar = 3;
    require(grid(window).getPixelAt(40, 10) != background, "Grid ignores the source meter");
}
void alignedHistoryAndEdges()
{
    SourceHistory history;
    auto packet = std::make_unique<CapturePacket>(); packet->header.sampleCount = 2048;
    for (int block = 0; block < 380; ++block)
    {
        for (int i = 0; i < 2048; ++i)
        { const auto value = static_cast<float>((block * 2048 + i) % 997) / 300;
          packet->left[static_cast<std::size_t>(i)] = value; packet->right[static_cast<std::size_t>(i)] = -value * 0.7f; }
        history.append(*packet, -1307 + block * 2048);
    }
    ViewLane lane; lane.history = &history; lane.begin = history.beginSample(); lane.end = history.endSample();
    for (int level = 0; level < 9; ++level)
    {
        const auto step = static_cast<std::int64_t>(SourceHistory::baseBinSamples) << level;
        for (auto first = lane.begin - step; first < lane.end; first += 57137)
        {
            const auto id = SourceHistory::floorBin(first, step);
            const auto actual = history.alignedBin(level, id);
            SampleExtrema expected;
            for (auto sample = std::max(lane.begin, id * step); sample < std::min(lane.end, (id+1)*step); ++sample) expected.add(history.at(sample));
            require(actual.present == expected.present && actual.low == expected.low && actual.high == expected.high,
                "Aligned history changed extrema across ring wrap/partial boundaries");
        }
    }
    history.clear(); packet->header.sampleCount = 128; packet->left.fill(0); packet->right.fill(0);
    packet->left[31] = 9; history.append(*packet, -17);
    lane.begin = history.beginSample(); lane.end = history.endSample();
    for (double first : { -30.3, 13.9, 14.0, 14.1, 15.0 })
    {
        const auto exact = WaveformRenderer::analyse(lane, first, first+64, 4, ChannelMode::midSide);
        const auto aligned = WaveformRenderer::analyseAligned(lane, first, first+64, 4, ChannelMode::midSide);
        require(exact.peaks == aligned.peaks && juce::exactlyEqual(exact.originalPeak, aligned.originalPeak),
            "Aligned geometry leaked an offscreen impulse into the exact peak readout");
    }
    const auto a = WaveformRenderer::analyseAligned(lane, -16, 80, 6, ChannelMode::left);
    const auto b = WaveformRenderer::analyseAligned(lane, -15.75, 80.25, 6, ChannelMode::left);
    require(a.buckets.front().high == b.buckets.front().high && std::abs(b.xOffset-a.xOffset+0.25f/16)<1e-6f,
        "Scrolling redefined completed waveform bins instead of translating them");
    // Missing callbacks are gaps, not silence; raw close zoom must not join them.
    packet->header.sampleCount = 8; packet->left.fill(1); packet->right.fill(-1);
    history.clear(); history.append(*packet, -8); history.append(*packet, 64);
    require(! history.extrema(0,64).present && ! history.hasSample(32), "Missing callback interval became fabricated audio");
    lane.begin = history.beginSample(); lane.end = history.endSample();
    const auto raw = WaveformRenderer::analyseAligned(lane,-8,72,100,ChannelMode::left);
    require(raw.individualSamples && raw.points.size()==16 && raw.points[8].x-raw.points[7].x>raw.xStep*2,
        "Close zoom dropped real samples or filled a missing interval");
    history.clear(); packet->left.fill(2); history.append(*packet,-8);
    require(juce::exactlyEqual(history.alignedBin(0,-1).high[0], 2.0f) && ! history.alignedBin(0,8).present,
        "Reset reused an old absolute bin ID");
    require(history.summaryBytes() <= SourceHistory::maximumSummaryBytes(), "Pyramid exceeded its reserved memory bound");
}
void hostMetadata()
{
    ScopeProcessor processor;
    juce::AudioProcessor::TrackProperties properties;
    properties.name = "Host kick"; properties.colourARGB = 0xff456789;
    processor.updateTrackProperties(properties);
    require(processor.captureSource()->displayName() == "Host kick" && processor.captureSource()->colour == 0xff456789,
            "Optional host name/color not used");
    properties.name.reset(); properties.colourARGB = 0xffabcdef; processor.updateTrackProperties(properties);
    require(processor.captureSource()->displayName() == "Host kick", "Color-only host update erased track name");
    properties.colourARGB.reset(); properties.name = "Host snare"; processor.updateTrackProperties(properties);
    require(processor.captureSource()->colour == 0xffabcdef, "Name-only host update erased track color");
    properties.name.reset(); processor.updateTrackProperties(properties);
    require(processor.captureSource()->displayName() == "Host snare" && processor.captureSource()->colour == 0xffabcdef,
            "Empty host update erased optional metadata");
    processor.captureSource()->nameOverride = "User kick";
    processor.captureSource()->colourOverridden = true; processor.captureSource()->colour = SourceState::palette[1];
    properties.name = "Renamed host"; properties.colourARGB = 0xff112233; processor.updateTrackProperties(properties);
    require(processor.captureSource()->displayName() == "User kick" && processor.captureSource()->colour == SourceState::palette[1],
            "Host updates replaced user name/color overrides");
}
void select(juce::Component& editor, const juce::String& name, int id)
{
    for (auto* child : descendants(editor))
    {
        if (auto* button = dynamic_cast<juce::Button*>(child); button && name == "Layout" && button->getName() == name)
        {
            button->setToggleState(id == 2, juce::dontSendNotification);
            button->onClick(); return;
        }
        if (auto* combo = dynamic_cast<juce::ComboBox*>(child); combo && combo->getName() == name)
        { combo->setSelectedId(id, juce::sendNotificationSync); return; }
    }
    throw std::runtime_error("Editor selector missing");
}
void settingsAndFreeze()
{
    auto source = SourceRegistry::instance().registerSource(); source->sampleRate.store(48000);
    auto& service = DisplayService::instance(); service.openViewer(source->id);
    {
        ViewerModel first(service), second(service);
        first.setChannelMode(ChannelMode::midSide); first.setLayout(ViewLayout::overlay); first.setHeadroomDb(7.3);
        require(second.channelMode() == ChannelMode::leftRight && second.selectedLayout() == ViewLayout::stacked
                && juce::exactlyEqual(second.selectedHeadroomDb(), 6.0), "Viewer channel/layout/headroom choices leaked");
        require(juce::exactlyEqual(first.selectedHeadroomDb(), 7.3), "Headroom still snaps to discrete presets");
        first.setHeadroomDb(std::numeric_limits<double>::quiet_NaN());
        require(juce::exactlyEqual(first.selectedHeadroomDb(), 7.3), "Non-finite headroom changed the display scale");
        first.setHeadroomDb(-1); require(juce::exactlyEqual(first.selectedHeadroomDb(), 0.0), "Headroom minimum not enforced");
        first.setHeadroomDb(25); require(juce::exactlyEqual(first.selectedHeadroomDb(), 24.0), "Headroom maximum not enforced");
        source->nameOverride = "Kick"; source->colour = SourceState::palette[3];
        require(first.lanes()[0].name == "Kick" && second.lanes()[0].colour == SourceState::palette[3], "Shared source name/color not reflected across viewers");
        CaptureHeader header; header.generation = service.captureState.load() >> 1; header.sequence = 1;
        std::array<float, 128> left {}, right {};
        left[33] = 1.5f; right[33] = -1.5f; left[127] = -1.5f; right[127] = 1.5f;
        require(source->queue.push(left.data(), right.data(), 128, header), "Freeze fixture push failed"); service.drain();
        require(first.freeze(), "Raw waveform freeze failed");
        first.setHeadroomDb(9.7);
        require(juce::exactlyEqual(first.selectedHeadroomDb(), 9.7), "Frozen headroom could not be adjusted");
        source->nameOverride = "Renamed kick"; source->colour = SourceState::palette[2];
        require(service.setClipThreshold(source->id, -3), "Frozen threshold edit failed");
        require(juce::exactlyEqual(first.lanes()[0].clipThresholdDb, -3.0), "Frozen viewer did not reflect source threshold");
        require(first.lanes()[0].name == "Renamed kick" && first.lanes()[0].colour == second.lanes()[0].colour,
                "Shared source metadata edits did not reach a frozen viewer");
        service.reset();
        const auto frozen = first.lanes()[0];
        require(frozen.snapshotExtrema != nullptr, "Freeze omitted bounded extrema cache");
        const auto mid = WaveformRenderer::analyse(frozen, 0, 128, 1, ChannelMode::mid);
        const auto stereo = WaveformRenderer::analyse(frozen, 0, 128, 1, ChannelMode::leftRight);
        require(juce::exactlyEqual(mid.peak(), 0.0f) && juce::exactlyEqual(mid.originalPeak, 1.5f) && juce::exactlyEqual(stereo.peak(), 1.5f), "Frozen raw audio could not be reprojected after Reset");
    }
    service.closeViewer(); SourceRegistry::instance().unregisterSource(source);
}
void compactEditors()
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
    std::vector<std::unique_ptr<ScopeProcessor>> processors;
    auto addTrack = [&](const char* name)
    {
        auto processor = std::make_unique<ScopeProcessor>();
        processor->prepareToPlay(48000, 2048); processor->setPlayHead(&playHead);
        juce::AudioProcessor::TrackProperties metadata; metadata.name = name;
        processor->updateTrackProperties(metadata); processors.push_back(std::move(processor));
    };
    addTrack("Kick");
    std::unique_ptr<juce::AudioProcessorEditor> editor(processors.front()->createEditor());
    auto& service = processors.front()->displayService();
    auto& headroom = [&]() -> juce::Slider&
    {
        for (auto* child : descendants(*editor))
            if (auto* slider = dynamic_cast<juce::Slider*>(child); slider && slider->getName() == "Headroom") return *slider;
        throw std::runtime_error("Continuous headroom control missing");
    }();
    auto verifyControls = [&]
    {
        std::vector<juce::Rectangle<int>> occupied;
        for (int n = 0; n < editor->getNumChildComponents(); ++n)
        {
            auto* child = editor->getChildComponent(n);
            if (! child->isVisible() || dynamic_cast<juce::ResizableCornerComponent*>(child)) continue;
            require(editor->getLocalBounds().contains(child->getBounds()), "Compact toolbar escaped the window");
            for (const auto& previous : occupied)
                require(! child->getBounds().intersects(previous), "Compact toolbar controls overlap");
            occupied.push_back(child->getBounds());
        }
        for (auto* child : descendants(*editor))
        {
            require(! child->getName().startsWith("Offset "), "Offset controls remain in the editor");
            if (auto* label = dynamic_cast<juce::Label*>(child))
                require(! label->getText().startsWith("Beats ") && label->getText() != "Live capture",
                        "Removed routine readouts remain in the editor");
            if (auto* button = dynamic_cast<juce::Button*>(child))
                require(! button->getButtonText().containsIgnoreCase("diagnostic")
                        && ! button->getButtonText().containsIgnoreCase("probe"), "Diagnostics remain in the editor");
        }
    };
    auto preview = [&](const char* name)
    {
        const auto directory = juce::SystemStats::getEnvironmentVariable("OSCILLOSCOPE_RENDER_PREVIEWS", {});
        if (directory.isEmpty()) return;
        juce::FileOutputStream output(juce::File(directory).getChildFile(name));
        require(output.openedOk() && output.setPosition(0) && output.truncate().wasOk(), "Compact preview output failed");
        juce::PNGImageFormat png;
        require(png.writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds(), true, 2), output),
                "Compact preview PNG failed");
    };
    juce::AudioBuffer<float> audio(2, 2048); juce::MidiBuffer midi;
    std::int64_t position = 0;
    auto capture = [&]
    {
        // Snapshot/knob checks pause this synthetic audio producer between cases.
        // Start each preview with fresh transport rather than simulating a stale tap.
        service.reset();
        for (int block = 0; block < 48; ++block)
        {
            playHead.position.setTimeInSamples(position); playHead.position.setPpqPosition(static_cast<double>(position) / 24000);
            for (std::size_t track = 0; track < processors.size(); ++track)
            {
                for (int sample = 0; sample < audio.getNumSamples(); ++sample)
                {
                    const auto time = static_cast<double>(position + sample) / 48000;
                    const auto envelope = std::exp(-std::fmod(time + static_cast<double>(track) * 0.08, 0.5) * 12);
                    audio.setSample(0, sample, static_cast<float>(1.5 * std::sin(time * (400 + static_cast<double>(track) * 140)) * envelope));
                    audio.setSample(1, sample, static_cast<float>(1.2 * std::cos(time * (430 + static_cast<double>(track) * 140)) * envelope));
                }
                processors[track]->processBlock(audio, midi);
            }
            position += audio.getNumSamples(); service.drain();
        }
        require(! service.held(), "Compact editor preview unexpectedly held capture");
    };
    require(editor->getWidth() <= 900 && editor->getHeight() <= 320, "Single-track editor still opens oversized");
    require(juce::exactlyEqual(headroom.getValue(), 6.0), "Headroom default changed");
    require(headroom.getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag, "Headroom is not a rotary knob");
    verifyControls(); capture(); preview("one-track.png");
    const auto before = editor->createComponentSnapshot(editor->getLocalBounds());
    headroom.setValue(7.3, juce::sendNotificationSync);
    require(std::abs(headroom.getValue() - 7.3) < 1e-8, "Headroom knob snaps to presets");
    const auto after = editor->createComponentSnapshot(editor->getLocalBounds());
    juce::Viewport* viewport = nullptr;
    for (auto* child : descendants(*editor))
        if (auto* candidate = dynamic_cast<juce::Viewport*>(child); candidate && candidate->getName() == "Tracks") viewport = candidate;
    require(viewport != nullptr, "Track viewport missing");
    int changed = 0;
    for (int y = viewport->getY(); y < viewport->getBottom(); ++y)
        for (int x = editor->getWidth() / 2; x < viewport->getRight(); ++x)
            if (before.getPixelAt(x, y) != after.getPixelAt(x, y)) ++changed;
    require(changed > 100, "Headroom knob did not change waveform scaling");
    std::unique_ptr<juce::AudioProcessorEditor> independent(processors.front()->createEditor());
    for (auto* child : descendants(*independent))
        if (auto* slider = dynamic_cast<juce::Slider*>(child); slider && slider->getName() == "Headroom")
            require(juce::exactlyEqual(slider->getValue(), 6.0), "Headroom knob leaked across viewers");
    const auto clipName = "Clip " + juce::String(processors.front()->captureSource()->id);
    auto clipKnob = [&](juce::Component& parent) -> juce::Slider&
    {
        for (auto* child : descendants(parent))
            if (auto* slider = dynamic_cast<juce::Slider*>(child); slider && slider->getName() == clipName) return *slider;
        throw std::runtime_error("Clip threshold knob missing");
    };
    auto& clip = clipKnob(*editor);
    require(clip.getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag, "Clip threshold is not a rotary knob");
    clip.setValue(-6.1, juce::sendNotificationSync);
    require(std::abs(processors.front()->captureSource()->clipThresholdDb + 6.1) < 1e-8
            && std::abs(clipKnob(*independent).getValue() + 6.1) < 1e-8, "Clip knob did not update the source and other viewer");
    clip.setValue(-99, juce::sendNotificationSync);
    require(juce::exactlyEqual(processors.front()->captureSource()->clipThresholdDb, -60.0), "Clip knob minimum not enforced");
    clip.setValue(99, juce::sendNotificationSync);
    require(juce::exactlyEqual(processors.front()->captureSource()->clipThresholdDb, 24.0), "Clip knob maximum not enforced");
    auto click = [&](const juce::String& text)
    {
        for (auto* child : descendants(*editor))
            if (auto* button = dynamic_cast<juce::Button*>(child); button && button->getButtonText() == text)
            { button->onClick(); return; }
        throw std::runtime_error("Freeze control missing");
    };
    click("Freeze");
    clip.setValue(-9.3, juce::sendNotificationSync);
    headroom.setValue(9.7, juce::sendNotificationSync);
    require(clip.isEnabled() && std::abs(processors.front()->captureSource()->clipThresholdDb + 9.3) < 1e-8,
            "Frozen clip knob could not be edited");
    preview("frozen-knobs.png"); click("Unfreeze");
    clip.setValue(0, juce::sendNotificationSync);
    require(juce::exactlyEqual(processors.front()->captureSource()->clipThresholdDb, 0.0),
            "Returning the clip knob to zero left a nonzero warning threshold");
    independent.reset();
    headroom.setValue(0, juce::sendNotificationSync); preview("zero-headroom.png");
    headroom.setValue(24, juce::sendNotificationSync); preview("maximum-headroom.png");
    headroom.setValue(6, juce::sendNotificationSync);
    addTrack("Bass"); service.drain(); capture();
    require(editor->getHeight() > 280 && editor->getHeight() <= 340, "Two-track editor failed to adapt its height");
    verifyControls(); preview("two-tracks.png");
    addTrack("Closed Hat"); service.drain(); capture();
    require(editor->getHeight() > 340 && editor->getHeight() <= 430, "Three-track editor failed to adapt its height");
    verifyControls(); preview("three-tracks.png");
    select(*editor, "Layout", 2); verifyControls(); preview("three-tracks-overlay.png");
    editor->setBoundsConstrained({ 0, 0, 760, 260 }); verifyControls(); preview("minimum-overlay.png");
    require(editor->getWidth() == 760 && editor->getHeight() == 260, "Compact manual window size was rejected");
    require(viewport->getViewedComponent()->getHeight() > viewport->getHeight(), "Small windows lost scrolling");
    const auto plotBeforeScroll = editor->createComponentSnapshot(editor->getLocalBounds());
    viewport->setViewPosition(0, viewport->getViewedComponent()->getHeight());
    const auto plotAfterScroll = editor->createComponentSnapshot(editor->getLocalBounds());
    for (int y = viewport->getY() + 10; y < viewport->getBottom() - 10; ++y)
        for (int x = editor->getWidth() / 2; x < viewport->getRight() - 20; ++x)
            require(plotBeforeScroll.getPixelAt(x, y) == plotAfterScroll.getPixelAt(x, y),
                    "Scrolling the track rail displaced the shared overlay plot");
    viewport->setViewPosition(0, 0);
    addTrack("Clap"); service.drain();
    require(editor->getWidth() == 760 && editor->getHeight() == 260, "Adding tracks discarded a manual window size");
    select(*editor, "Layout", 1); verifyControls();
    require(editor->getHeight() == 260, "Changing layout discarded a manual window size");
    // A fresh viewer resumes automatic sizing and shrinks as tracks disappear.
    editor.reset(); processors.pop_back();
    editor.reset(processors.front()->createEditor());
    processors.pop_back(); service.drain();
    require(editor->getHeight() > 280 && editor->getHeight() <= 340, "Removing tracks did not shrink an automatic window");
    processors.pop_back(); service.drain();
    require(editor->getHeight() <= 320, "Returning to one track retained a large window");
}
void benchmarkEditors()
{
    class PlayHead final : public juce::AudioPlayHead
    {
    public:
        juce::Optional<PositionInfo> getPosition() const override { return position; }
        PositionInfo position;
    } playHead;
    playHead.position.setIsPlaying(true); playHead.position.setBpm(60);
    playHead.position.setTimeSignature(juce::AudioPlayHead::TimeSignature { 4, 4 });
    playHead.position.setPpqPositionOfLastBarStart(0);
    std::vector<std::unique_ptr<ScopeProcessor>> processors;
    for (int n = 0; n < 8; ++n)
    { processors.push_back(std::make_unique<ScopeProcessor>()); processors.back()->prepareToPlay(48000, 2048); processors.back()->setPlayHead(&playHead); }
    const char* names[] { "Kick", "Bass", "Clap", "Closed Hat", "Shaker", "Percussion", "Synth", "Bus" };
    for (std::size_t n = 0; n < processors.size(); ++n)
    { juce::AudioProcessor::TrackProperties metadata; metadata.name = names[n]; processors[n]->updateTrackProperties(metadata); }
    std::unique_ptr<juce::AudioProcessorEditor> a(processors[0]->createEditor()), b(processors[1]->createEditor());
    auto& service = processors[0]->displayService();
    // Set each viewer to the full 16-second four-bar window at 60 BPM.
    for (auto* editor : { a.get(), b.get() })
    {
        for (auto* child : descendants(*editor))
            if (auto* combo = dynamic_cast<juce::ComboBox*>(child); combo && combo->getNumItems() == 5)
                combo->setSelectedId(5, juce::sendNotificationSync);
        select(*editor, "Channels", 6);
    }
    select(*a, "Channels", 4);
    int channelControls = 0;
    for (auto* child : descendants(*a))
        if (auto* combo = dynamic_cast<juce::ComboBox*>(child); combo && combo->getName().startsWith("Channel"))
        { ++channelControls; require(combo->getSelectedId() == 4, "Global mono projection did not apply"); }
    require(channelControls == 1, "Channel projection still has per-track controls");
    for (auto* child : descendants(*b))
        if (auto* combo = dynamic_cast<juce::ComboBox*>(child); combo && combo->getName() == "Channels")
            require(combo->getSelectedId() == 6, "Global channel choice leaked to another viewer");
    select(*a, "Channels", 6);
    auto named = [](juce::Component& editor, const juce::String& name) -> juce::Component&
    {
        for (auto* child : descendants(editor)) if (child->getName() == name) return *child;
        throw std::runtime_error("Expected track control missing");
    };
    auto verifyRows = [&]
    {
        int previousBottom = -1;
        for (const auto& processor : processors)
        {
            const auto id = juce::String(processor->captureSource()->id);
            auto& track = named(*a, "Track " + id);
            auto& clip = named(*a, "Clip " + id);
            require(track.getY() > previousBottom && clip.getY() >= track.getBottom(),
                    "Track controls escaped their own row or overlap another track");
            require(clip.getParentComponent() == track.getParentComponent(),
                    "Clip threshold escaped the track row");
            previousBottom = clip.getBottom();
        }
    };
    verifyRows();
    a->setSize(960, 700); verifyRows();
    juce::Viewport* viewport = nullptr;
    for (auto* child : descendants(*a)) if (auto* candidate = dynamic_cast<juce::Viewport*>(child); candidate && candidate->getName() == "Tracks") viewport = candidate;
    require(viewport && viewport->getViewedComponent()->getHeight() > viewport->getHeight(),
            "Short windows do not preserve readable, scrollable track rows");
    viewport->setViewPosition(0, viewport->getViewedComponent()->getHeight());
    require(viewport->getViewPositionY() > 0, "Last track is unreachable in a short window");
    viewport->setViewPosition(0, 0); a->setSize(1080, 940);
    select(*a, "Layout", 2); verifyRows(); select(*a, "Layout", 1);
    auto& visibility = dynamic_cast<juce::ToggleButton&>(named(*a, "Visible " + juce::String(processors[0]->captureSource()->id)));
    const auto secondTrackY = named(*a, "Track " + juce::String(processors[1]->captureSource()->id)).getY();
    visibility.setToggleState(false, juce::dontSendNotification); visibility.onClick();
    require(named(*a, "Track " + juce::String(processors[1]->captureSource()->id)).getY() == secondTrackY,
            "Hiding a source moved another source's controls away from its waveform");
    visibility.setToggleState(true, juce::dontSendNotification); visibility.onClick();
    juce::AudioBuffer<float> audio(2, 2048); juce::MidiBuffer midi;
    for (int n = 0; n < 2048; ++n)
    { audio.setSample(0, n, static_cast<float>(1.5 * std::sin(n * 0.017))); audio.setSample(1, n, static_cast<float>(1.2 * std::cos(n * 0.023))); }
    std::int64_t position = 0;
    bool denseSignal = false;
    auto capture = [&]
    {
        playHead.position.setTimeInSamples(position); playHead.position.setPpqPosition(static_cast<double>(position) / 48000);
        for (std::size_t source = 0; source < processors.size(); ++source)
        {
            for (int n = 0; n < audio.getNumSamples(); ++n)
            {
                const double sourceNumber = static_cast<double>(source);
                const double samplePosition = static_cast<double>(position + n);
                const double beat = static_cast<double>(position + n) / 48000;
                const double phase = std::fmod(beat + sourceNumber * 0.12, 1 + sourceNumber * 0.25);
                const double envelope = denseSignal ? 1 : std::exp(-phase * (4 + sourceNumber));
                audio.setSample(0, n, static_cast<float>((1.6 - sourceNumber * 0.1) * std::sin(samplePosition * (0.013 + sourceNumber * 0.007)) * envelope));
                audio.setSample(1, n, static_cast<float>((1.3 - sourceNumber * 0.08) * std::cos(samplePosition * (0.017 + sourceNumber * 0.005)) * envelope));
            }
            processors[source]->processBlock(audio, midi);
        }
        position += audio.getNumSamples(); service.drain();
    };
    for (int block = 0; block < 376; ++block) capture();
    for (const auto& processor : processors)
        require(processor->captureSource()->history.size() == 768000, "Benchmark did not fill eight full 16-second histories");
    require(! service.held(), "Benchmark steady playback unexpectedly held");
    // Include shared ingestion, listener updates and both actual JUCE paints.
    // Snapshot allocation/rasterization is also inside the measured interval.
    {
        ViewerModel probe(service); probe.setWindow(BeatWindow::fourBars);
        const auto window = probe.window(); const auto lanes = probe.lanes();
        auto start = juce::Time::getMillisecondCounterHiRes();
        std::vector<WaveformFrame> frames;
        for (const auto& lane : lanes)
            frames.push_back(WaveformRenderer::analyse(lane, lane.anchor.sampleAt(window.begin), lane.anchor.sampleAt(window.end), 880, ChannelMode::midSide));
        const auto scanEnd = juce::Time::getMillisecondCounterHiRes();
        juce::Image image(juce::Image::ARGB, 880, 400, true); juce::Graphics graphics(image);
        for (const auto& frame : frames) WaveformRenderer::draw(graphics, { 0, 0, 880, 50 }, frame, juce::Colours::cyan, 2);
        std::cerr << "PROFILE eight-lane analysis=" << scanEnd - start << " draw=" << juce::Time::getMillisecondCounterHiRes() - scanEnd << " ms\n";
    }
    audio.setSize(2, 800, true, false, true);
    // Keep the existing 1x/60 Hz gate. Also measure Retina costs explicitly;
    // 2x snapshots are an additional stress measurement, not a host FPS claim.
    for (const float snapshotScale : { 1.0f, 2.0f })
    for (int layout : { 1, 2 })
    {
        select(*a, "Layout", layout); select(*b, "Layout", layout);
        for (int warmup = 0; warmup < 3; ++warmup)
        { capture(); a->createComponentSnapshot(a->getLocalBounds(), true, snapshotScale); b->createComponentSnapshot(b->getLocalBounds(), true, snapshotScale); }
        std::vector<double> times; double ingestMs = 0, firstMs = 0, secondMs = 0;
        for (int frame = 0; frame < 20; ++frame)
        {
            const auto start = juce::Time::getMillisecondCounterHiRes();
            capture(); const auto t1 = juce::Time::getMillisecondCounterHiRes();
            a->createComponentSnapshot(a->getLocalBounds(), true, snapshotScale); const auto t2 = juce::Time::getMillisecondCounterHiRes();
            b->createComponentSnapshot(b->getLocalBounds(), true, snapshotScale); const auto t3 = juce::Time::getMillisecondCounterHiRes();
            ingestMs += t1 - start; firstMs += t2 - t1; secondMs += t3 - t2;
            times.push_back(juce::Time::getMillisecondCounterHiRes() - start);
        }
        std::cerr << "PROFILE ingestion=" << ingestMs / 20 << " first=" << firstMs / 20 << " second=" << secondMs / 20 << " ms\n";
        std::sort(times.begin(), times.end()); double sum = 0; for (auto time : times) sum += time;
        std::cout << "BENCHMARK " << (layout == 1 ? "stacked" : "overlay") << " " << snapshotScale << "x eight stereo/16s/48k/two viewers M+S: mean="
                  << sum / static_cast<double>(times.size()) << " ms p95=" << times[18] << " ms max=" << times.back() << " ms\n";
        const auto previewDir = juce::SystemStats::getEnvironmentVariable("OSCILLOSCOPE_RENDER_PREVIEWS", {});
        if (previewDir.isNotEmpty())
        {
            juce::FileOutputStream output(juce::File(previewDir).getChildFile(juce::String(layout == 1 ? "viewer-stacked" : "viewer-overlay") + (snapshotScale > 1 ? "-retina.png" : ".png")));
            require(output.openedOk() && output.setPosition(0) && output.truncate().wasOk(), "Preview output failed");
            juce::PNGImageFormat png; require(png.writeImageToStream(a->createComponentSnapshot(a->getLocalBounds(), true, snapshotScale), output), "Preview PNG failed");
        }
#ifdef NDEBUG
        if (snapshotScale <= 1)
            require(times[18] < 1000.0 / DisplayService::framesPerSecond, "Release eight-source two-view rendering exceeded 60 fps frame budget");
#endif
    }
    // Dense sustained signals exercise the maximum filled-envelope cost, in
    // addition to the distinct transient signals used for readable previews.
    denseSignal = true; audio.setSize(2, 2048, false, false, true);
    for (int block = 0; block < 376; ++block) capture();
    audio.setSize(2, 800, true, false, true);
    for (const float snapshotScale : { 1.0f, 2.0f })
    for (int layout : { 1, 2 })
    {
        select(*a, "Layout", layout); select(*b, "Layout", layout);
        std::vector<double> times;
        for (int frame = 0; frame < 20; ++frame)
        {
            const auto start = juce::Time::getMillisecondCounterHiRes(); capture();
            a->createComponentSnapshot(a->getLocalBounds(), true, snapshotScale); b->createComponentSnapshot(b->getLocalBounds(), true, snapshotScale);
            times.push_back(juce::Time::getMillisecondCounterHiRes() - start);
        }
        std::sort(times.begin(), times.end());
        std::cout << "BENCHMARK dense " << (layout == 1 ? "stacked" : "overlay") << " " << snapshotScale << "x p95=" << times[18] << " ms max=" << times.back() << " ms\n";
#ifdef NDEBUG
        if (snapshotScale <= 1)
            require(times[18] < 1000.0 / DisplayService::framesPerSecond, "Dense eight-source two-viewer rendering exceeded 60 fps budget");
#endif
    }
    // Restore distinct transient histories for the focused overlay preview.
    denseSignal = false; audio.setSize(2, 2048, false, false, true);
    for (int block = 0; block < 376; ++block) capture();
    select(*a, "Overlay focus", 2);
    require(service.setClipThreshold(processors[0]->captureSource()->id, -3), "Could not set editor threshold");
    bool otherUnfocused = false;
    for (auto* child : descendants(*b))
        if (auto* combo = dynamic_cast<juce::ComboBox*>(child); combo && combo->getName() == "Overlay focus")
            otherUnfocused = combo->getSelectedId() == 1;
    require(otherUnfocused, "Overlay focus leaked across viewers");
    auto& clip = dynamic_cast<juce::Slider&>(named(*a, "Clip " + juce::String(processors[0]->captureSource()->id)));
    clip.setValue(-6, juce::sendNotificationSync);
    require(std::abs(processors[0]->captureSource()->clipThresholdDb + 6.0) < 1e-8, "Editor clip knob did not apply immediately");
    const auto previewDir = juce::SystemStats::getEnvironmentVariable("OSCILLOSCOPE_RENDER_PREVIEWS", {});
    if (previewDir.isNotEmpty())
    {
        juce::FileOutputStream output(juce::File(previewDir).getChildFile("viewer-overlay-focus.png"));
        require(output.openedOk() && output.setPosition(0) && output.truncate().wasOk(), "Focus preview output failed");
        juce::PNGImageFormat png; require(png.writeImageToStream(a->createComponentSnapshot(a->getLocalBounds()), output), "Focus preview PNG failed");
    }
    require(service.visualizationBytes() < DisplayService::visualizationBudgetBytes, "Rendering exceeded visualization budget");
}
}
int main()
{
    try
    {
        juce::ScopedJuceInitialiser_GUI juce;
        projectionsAndPeaks(); peakPreservationAndCoordinates(); highlightAndOverflow(); cachedExtrema(); thresholdsAndOverlay(); stableTraceAndGrid(); alignedHistoryAndEdges(); hostMetadata(); settingsAndFreeze(); compactEditors(); benchmarkEditors();
        require(SourceRegistry::instance().sources().empty(), "Rendering tests leaked sources");
        std::cout << "PASS: L/R and M/S projection/extrema; full-window one-sample peaks; headroom/levels; highlights/overflow; "
                     "independent viewer settings; shared colors/names; raw freezes; real eight-source/two-editor benchmark\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
