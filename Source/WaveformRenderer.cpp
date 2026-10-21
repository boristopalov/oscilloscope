#include "WaveformRenderer.h"
#include "ScopeLookAndFeel.h"

namespace oscilloscope
{
WaveformFrame WaveformRenderer::analyse(const ViewLane& lane, double start, double end, int pixels, ChannelMode mode)
{
    WaveformFrame frame; frame.mode = mode;
    if (pixels <= 0 || ! std::isfinite(start) || ! std::isfinite(end) || end <= start || lane.end <= lane.begin) return frame;
    frame.buckets.resize(static_cast<std::size_t>(pixels));
    const double perPixel = (end - start) / pixels;
    frame.individualSamples = perPixel <= 1;
    if (frame.individualSamples) frame.xStep = static_cast<float>(1 / perPixel);
    const auto* viewport = ! frame.individualSamples && lane.history != nullptr && pixels <= SourceHistory::maxCachedColumns
        ? &lane.history->bucketExtrema(start, end, pixels) : nullptr;
    if (frame.individualSamples) frame.points.reserve(static_cast<std::size_t>(pixels + 1));
    for (int x = 0; x < pixels; ++x)
    {
        const double first = start + perPixel * x, last = x + 1 == pixels ? end : start + perPixel * (x + 1);
        const auto a = static_cast<std::int64_t>(std::clamp(std::ceil(first), static_cast<double>(lane.begin), static_cast<double>(lane.end)));
        const auto b = static_cast<std::int64_t>(std::clamp(std::ceil(last), static_cast<double>(lane.begin), static_cast<double>(lane.end)));
        auto& bucket = frame.buckets[static_cast<std::size_t>(x)];
        if (! frame.individualSamples)
        {
            const auto extrema = viewport != nullptr ? (*viewport)[static_cast<std::size_t>(x)] : lane.extrema(a, b);
            bucket.present = extrema.present;
            if (! extrema.present) continue;
            const std::array<int, 2> channels = mode == ChannelMode::midSide ? std::array<int, 2> { 2, 3 }
                : mode == ChannelMode::leftRight ? std::array<int, 2> { 0, 1 }
                : std::array<int, 2> { mode == ChannelMode::left ? 0 : mode == ChannelMode::right ? 1 : mode == ChannelMode::mid ? 2 : 3, 0 };
            frame.originalPeak = std::max(frame.originalPeak, std::max({ std::abs(extrema.low[0]), std::abs(extrema.high[0]), std::abs(extrema.low[1]), std::abs(extrema.high[1]) }));
            for (int t = 0; t < ChannelProjector::traceCount(mode); ++t)
            {
                const auto trace = static_cast<std::size_t>(t), channel = static_cast<std::size_t>(channels[trace]);
                bucket.low[trace] = extrema.low[channel]; bucket.high[trace] = extrema.high[channel];
                frame.peaks[trace] = std::max(frame.peaks[trace], std::max(std::abs(bucket.low[trace]), std::abs(bucket.high[trace])));
            }
            continue;
        }
        for (auto s = a; s < b; ++s)
        {
            if (! lane.hasSample(s)) continue;
            auto raw = lane.at(s);
            // Capture sanitizes already; remain robust for synthetic/frozen input.
            if (! std::isfinite(raw.left)) raw.left = 0;
            if (! std::isfinite(raw.right)) raw.right = 0;
            frame.originalPeak = std::max(frame.originalPeak, std::max(std::abs(raw.left), std::abs(raw.right)));
            const auto values = ChannelProjector::project(raw, mode);
            for (int t = 0; t < ChannelProjector::traceCount(mode); ++t)
            {
                const auto i = static_cast<std::size_t>(t);
                if (! bucket.present) bucket.low[i] = bucket.high[i] = values[i];
                else { bucket.low[i] = std::min(bucket.low[i], values[i]); bucket.high[i] = std::max(bucket.high[i], values[i]); }
                frame.peaks[i] = std::max(frame.peaks[i], std::abs(values[i]));
            }
            bucket.present = true;
            if (frame.individualSamples)
                frame.points.push_back({ static_cast<float>((static_cast<double>(s) - start) / perPixel), values });
        }
    }
    return frame;
}
WaveformFrame WaveformRenderer::analyseAligned(const ViewLane& lane, double start, double end, int pixels, ChannelMode mode)
{
    WaveformFrame frame; frame.mode = mode;
    if (pixels <= 0 || ! std::isfinite(start) || ! std::isfinite(end) || end <= start || lane.end <= lane.begin
        || std::abs(start) > 9.0e15 || std::abs(end) > 9.0e15) return frame;
    const double perPixel = (end - start) / pixels;
    const std::array<int, 2> channels = mode == ChannelMode::midSide ? std::array<int, 2> { 2, 3 }
        : mode == ChannelMode::leftRight ? std::array<int, 2> { 0, 1 }
        : std::array<int, 2> { mode == ChannelMode::left ? 0 : mode == ChannelMode::right ? 1 : mode == ChannelMode::mid ? 2 : 3, 0 };
    const auto firstSample = static_cast<std::int64_t>(std::clamp(std::ceil(start), static_cast<double>(lane.begin), static_cast<double>(lane.end)));
    const auto lastSample = static_cast<std::int64_t>(std::clamp(std::ceil(end), static_cast<double>(lane.begin), static_cast<double>(lane.end)));
    const auto peaks = lane.extrema(firstSample, lastSample);
    if (peaks.present)
    {
        frame.originalPeak = std::max({ std::abs(peaks.low[0]), std::abs(peaks.high[0]), std::abs(peaks.low[1]), std::abs(peaks.high[1]) });
        for (int t = 0; t < ChannelProjector::traceCount(mode); ++t)
        { const auto c = static_cast<std::size_t>(channels[static_cast<std::size_t>(t)]);
          frame.peaks[static_cast<std::size_t>(t)] = std::max(std::abs(peaks.low[c]), std::abs(peaks.high[c])); }
    }
    if (perPixel < SourceHistory::baseBinSamples)
    {
        frame.individualSamples = true; frame.xStep = static_cast<float>(1 / perPixel);
        frame.points.reserve(static_cast<std::size_t>(std::max<std::int64_t>(0, lastSample - firstSample)));
        for (auto sample = firstSample; sample < lastSample; ++sample)
            if (lane.hasSample(sample)) frame.points.push_back({ static_cast<float>((static_cast<double>(sample) - start) / perPixel), ChannelProjector::project(lane.at(sample), mode) });
        return frame;
    }
    int level = 0;
    while (level + 1 < SourceHistory::levelCount && (SourceHistory::baseBinSamples << (level + 1)) <= perPixel) ++level;
    const auto step = static_cast<std::int64_t>(SourceHistory::baseBinSamples) << level;
    const auto first = static_cast<std::int64_t>(std::floor(start / static_cast<double>(step)));
    const auto last = static_cast<std::int64_t>(std::ceil(end / static_cast<double>(step)));
    frame.xOffset = static_cast<float>((static_cast<double>(first * step) - start) / perPixel);
    frame.xStep = static_cast<float>(static_cast<double>(step) / perPixel);
    frame.buckets.reserve(static_cast<std::size_t>(last - first));
    for (auto id = first; id < last; ++id)
    {
        const auto value = lane.history ? lane.history->alignedBin(level, id) : lane.extrema(id * step, (id + 1) * step);
        WaveformBucket bucket; bucket.present = value.present;
        for (int t = 0; t < ChannelProjector::traceCount(mode); ++t)
        { const auto trace = static_cast<std::size_t>(t), channel = static_cast<std::size_t>(channels[trace]);
          bucket.low[trace] = value.low[channel]; bucket.high[trace] = value.high[channel]; }
        frame.buckets.push_back(bucket);
    }
    return frame;
}
float WaveformRenderer::amplitudeRange(double headroomDb) noexcept
{ return static_cast<float>(std::pow(10.0, headroomDb / 20)); }
double WaveformRenderer::peakDb(float magnitude) noexcept
{ return magnitude > 0 ? 20 * std::log10(static_cast<double>(magnitude)) : -std::numeric_limits<double>::infinity(); }
juce::String WaveformRenderer::peakText(float magnitude)
{
    if (magnitude <= 0) return juce::String::fromUTF8("−∞ dBFS");
    const auto db = peakDb(magnitude);
    return (db > 0 ? "+" : "") + juce::String(db, 2) + " dBFS";
}
float WaveformRenderer::yFor(float value, juce::Rectangle<float> area, float range) noexcept
{ return area.getCentreY() - std::clamp(value / range, -1.0f, 1.0f) * area.getHeight() * 0.5f; }
void WaveformRenderer::drawGrid(juce::Graphics& g, juce::Rectangle<float> area, const ViewWindow& window, bool synced, float range)
{
    g.setColour(scopeStyle::plot); g.fillRect(area);
    if (synced && window.end > window.begin && std::isfinite(window.beatsPerBar) && window.beatsPerBar > 0)
    {
        const double step = window.beatsPerBar;
        // A duration ruler, anchored to the newest edge of the plot. Only whole-bar
        // intervals are marked; host PPQ and loop phase never move the grid.
        const double span = window.end - window.begin;
        for (int line = 0; line < 4096; ++line)
        {
            const double age = line * step;
            if (age > span) break;
            const auto x = area.getRight() - static_cast<float>(age / span) * area.getWidth();
            g.setColour(scopeStyle::barRule);
            g.drawVerticalLine(static_cast<int>(x), area.getY(), area.getBottom());
        }
    }
    for (const float value : { 0.0f, 1.0f, -1.0f })
    {
        g.setColour(juce::exactlyEqual(value, 0.0f) ? scopeStyle::zeroRule : scopeStyle::fullScaleRule);
        g.drawHorizontalLine(static_cast<int>(yFor(value, area, range)), area.getX(), area.getRight());
    }
}
void WaveformRenderer::drawReferences(juce::Graphics& g, juce::Rectangle<float> area, float range)
{
    g.setFont(juce::FontOptions(10.0f));
    g.setColour(scopeStyle::muted);
    g.drawText("+1 / 0 dBFS", area.withY(std::max(area.getY(), yFor(1, area, range) - 14)).withHeight(14), juce::Justification::topRight);
    g.drawText("-1 / 0 dBFS", area.withY(std::min(area.getBottom() - 14, yFor(-1, area, range))).withHeight(14), juce::Justification::topRight);
}
void WaveformRenderer::rasterize(juce::Image& raster, const WaveformFrame& frame, juce::Colour colour, float range, float threshold, bool overlay, float pixelScale)
{
    if (! raster.isValid() || frame.buckets.empty()) return;
    const auto area = raster.getBounds().toFloat();
    const int width = std::min(raster.getWidth(), static_cast<int>(frame.buckets.size())), height = raster.getHeight();
    const auto over = juce::Colour(0xfff08065);
    juce::Image::BitmapData bitmap(raster, juce::Image::BitmapData::readWrite);
    auto pixelFor = [](juce::Colour ink)
    {
        juce::PixelARGB pixel;
        pixel.setARGB(ink.getAlpha(), ink.getRed(), ink.getGreen(), ink.getBlue());
        pixel.premultiply(); return pixel;
    };
    auto fill = [&](int x, float low, float high, juce::PixelARGB ink)
    {
        const auto first = std::clamp(static_cast<int>(std::floor(yFor(high, area, range))), 0, height - 1);
        const auto last = std::clamp(static_cast<int>(std::ceil(yFor(low, area, range))), first + 1, height);
        auto* pointer = bitmap.getPixelPointer(x, first);
        for (int y = first; y < last; ++y, pointer += bitmap.lineStride)
            if (ink.getAlpha() == 255) *reinterpret_cast<juce::PixelARGB*>(pointer) = ink;
            else reinterpret_cast<juce::PixelARGB*>(pointer)->blend(ink);
    };
    for (int t = 0; t < ChannelProjector::traceCount(frame.mode); ++t)
    {
        const auto trace = static_cast<std::size_t>(t);
        const auto traceColour = t == 0 ? colour : colour.brighter(0.25f);
        const auto interior = pixelFor(scopeStyle::plot.interpolatedWith(traceColour, t == 0 ? 0.38f : 0.24f)
            .withAlpha(colour.getFloatAlpha()));
        const auto overPixel = pixelFor(over.withAlpha(colour.getFloatAlpha()));
        const auto strongNormal = pixelFor(traceColour.withAlpha((overlay ? 0.85f : 0.95f) * colour.getFloatAlpha()));
        const auto strongWarning = pixelFor(over.withAlpha((overlay ? 0.85f : 0.95f) * colour.getFloatAlpha()));
        const float upperThreshold = area.getCentreY() - threshold / range * area.getHeight() * 0.5f;
        const float lowerThreshold = area.getCentreY() + threshold / range * area.getHeight() * 0.5f;
        for (int x = 0; x < width; ++x)
        {
            const auto& bucket = frame.buckets[static_cast<std::size_t>(x)];
            if (! bucket.present) continue;
            const float low = bucket.low[trace], high = bucket.high[trace];
            // Stacked plots have one subdued envelope beneath both contours.
            // Overlays use contours so
            // dense signals cannot cover the other sources with opaque fills.
            if (! overlay)
            {
                if (t == 0 && low <= threshold && high >= -threshold)
                    fill(x, std::max(low, -threshold), std::min(high, threshold), interior);
                if (high > threshold) fill(x, std::max(low, threshold), high, overPixel);
                if (low < -threshold) fill(x, low, std::min(high, -threshold), overPixel);
            }
        }
        // Draw connected min/max contours in pixel space. Interpolating at the
        // column boundaries joins adjacent extrema; 3-point strokes with fractional
        // edge coverage avoid the old isolated-dot / one-pixel staircase look.
        // Work scales with display width rather than captured sample count.
        for (int x = 0; x < width; ++x)
        {
            const auto& bucket = frame.buckets[static_cast<std::size_t>(x)];
            if (! bucket.present) continue;
            auto contour = [&](bool upper)
            {
                const auto value = [&](const WaveformBucket& b) { return upper ? b.high[trace] : b.low[trace]; };
                const float centre = yFor(value(bucket), area, range);
                float left = centre, right = centre;
                if (x > 0 && frame.buckets[static_cast<std::size_t>(x - 1)].present)
                    left = (centre + yFor(value(frame.buckets[static_cast<std::size_t>(x - 1)]), area, range)) * 0.5f;
                if (x + 1 < width && frame.buckets[static_cast<std::size_t>(x + 1)].present)
                    right = (centre + yFor(value(frame.buckets[static_cast<std::size_t>(x + 1)]), area, range)) * 0.5f;
                const float top = std::min({ left, centre, right }) - 1.5f * pixelScale;
                const float bottom = std::max({ left, centre, right }) + 1.5f * pixelScale;
                const int first = std::clamp(static_cast<int>(std::floor(top)), 0, height - 1);
                const int last = std::clamp(static_cast<int>(std::ceil(bottom)), first + 1, height);
                // Solid contours keep a moving feature at constant opacity.
                auto* pointer = bitmap.getPixelPointer(x, first);
                for (int y = first; y < last; ++y, pointer += bitmap.lineStride)
                {
                    const float coverage = std::min(1.0f, std::max(0.0f,
                        std::min(bottom, static_cast<float>(y + 1)) - std::max(top, static_cast<float>(y))));
                    const bool excursion = (static_cast<float>(y) + 0.5f < upperThreshold
                        || static_cast<float>(y) + 0.5f > lowerThreshold)
                        && (bucket.high[trace] > threshold || bucket.low[trace] < -threshold);
                    const auto ink = excursion ? strongWarning : strongNormal;
                    auto* pixel = reinterpret_cast<juce::PixelARGB*>(pointer);
                    if (coverage >= 1) pixel->blend(ink);
                    else pixel->blend(ink, static_cast<juce::uint32>(std::round(coverage * 256)));
                }
            };
            contour(true);
            if (! juce::exactlyEqual(bucket.low[trace], bucket.high[trace])) contour(false);
            // Fixed-range overflow remains explicit and independent of clipping.
            if (bucket.high[trace] > range)
                fill(x, range * (1 - 6.0f * pixelScale / static_cast<float>(height)), range,
                     pixelFor((bucket.high[trace] > threshold ? over : traceColour).withAlpha(colour.getFloatAlpha())));
            if (bucket.low[trace] < -range)
                fill(x, -range, -range * (1 - 6.0f * pixelScale / static_cast<float>(height)),
                     pixelFor((bucket.low[trace] < -threshold ? over : traceColour).withAlpha(colour.getFloatAlpha())));
        }
    }
}
void WaveformRenderer::draw(juce::Graphics& g, juce::Rectangle<float> area, const WaveformFrame& frame, juce::Colour colour, float range, float threshold, float pixelScale)
{
    if (area.isEmpty() || frame.buckets.empty()) return;
    juce::Graphics::ScopedSaveState save(g); g.reduceClipRegion(area.toNearestInt());
    const auto over = juce::Colour(0xfff08065).withAlpha(colour.getFloatAlpha());
    auto line = [&](juce::Point<float> a, juce::Point<float> b, juce::Colour ink)
    {
        g.setColour(ink);
        g.drawLine({ a, b }, 3);
    };
    // Split at the source threshold so only excursions turn red.
    auto segment = [&](float x1, float v1, float x2, float v2, int trace)
    {
        std::array<float, 4> cuts { 0, 1, 1, 1 }; int count = 2;
        for (float boundary : { -threshold, threshold })
            if ((v1 < boundary && v2 > boundary) || (v2 < boundary && v1 > boundary))
                cuts[static_cast<std::size_t>(count++)] = static_cast<float>((static_cast<double>(boundary) - v1) / (static_cast<double>(v2) - v1));
        std::sort(cuts.begin(), cuts.begin() + count);
        for (int part = 1; part < count; ++part)
        {
            const float t1 = cuts[static_cast<std::size_t>(part - 1)], t2 = cuts[static_cast<std::size_t>(part)];
            const auto value = [&](float t) { return static_cast<float>((1.0 - t) * v1 + static_cast<double>(t) * v2); };
            const auto midpoint = value((t1 + t2) * 0.5f);
            line({ x1 + (x2 - x1) * t1, yFor(value(t1), area, range) },
                 { x1 + (x2 - x1) * t2, yFor(value(t2), area, range) },
                 std::abs(midpoint) > threshold ? over : trace == 0 ? colour : colour.brighter(0.22f));
        }
    };
    if (! frame.individualSamples)
    {
        juce::Image raster(juce::Image::ARGB, static_cast<int>(frame.buckets.size()),
            std::max(1, static_cast<int>(std::ceil(area.getHeight() * pixelScale))), true, juce::NativeImageType());
        rasterize(raster, frame, colour, range, threshold, false, pixelScale);
        g.drawImageTransformed(raster, juce::AffineTransform::scale(1 / pixelScale).translated(area.getX(), area.getY()));
        return;
    }
    for (int t = 0; t < ChannelProjector::traceCount(frame.mode); ++t)
    {
        const auto trace = static_cast<std::size_t>(t);
        if (frame.individualSamples)
        {
            if (frame.points.size() == 1)
            {
                const auto& p = frame.points.front();
                segment(area.getX() + p.x / pixelScale - 1, p.values[trace], area.getX() + p.x / pixelScale + 1, p.values[trace], t);
            }
            for (std::size_t i = 0; i < frame.points.size(); ++i)
            {
                const auto& p = frame.points[i]; const float x = area.getX() + p.x / pixelScale;
                if (i > 0)
                {
                    const auto& previous = frame.points[i - 1];
                    if (p.x - previous.x <= frame.xStep * 1.01f)
                        segment(area.getX() + previous.x / pixelScale, previous.values[trace], x, p.values[trace], t);
                }
            }
        }
        for (std::size_t x = 0; x < frame.buckets.size(); ++x)
        {
            const auto& bucket = frame.buckets[x]; if (! bucket.present) continue;
            const auto px = area.getX() + static_cast<float>(x) / pixelScale;
            g.setColour(bucket.high[trace] > threshold ? over : colour);
            if (bucket.high[trace] > range) g.fillRect(px, area.getY(), 1.0f / pixelScale, 3.0f);
            g.setColour(bucket.low[trace] < -threshold ? over : colour);
            if (bucket.low[trace] < -range) g.fillRect(px, area.getBottom() - 3, 1.0f / pixelScale, 3.0f);
        }
    }
}
}
