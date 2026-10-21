#pragma once
#include "CaptureQueue.h"
#include "SampleExtrema.h"
#include <vector>

namespace oscilloscope
{
// Message-thread-only raw ring plus sample-aligned extrema. Completed bins keep
// their identity while scrolling; absolute IDs prevent wrap/reset aliasing.
class SourceHistory
{
public:
    static constexpr std::size_t capacitySamples = 16 * 48000;
    static constexpr std::size_t summarySamples = 32; // Frozen snapshot granularity.
    static constexpr int baseBinSamples = 8, levelCount = 18, maxCachedColumns = 4096;
    struct Bin { std::int64_t id = invalidId; SampleExtrema value; };
    static constexpr std::int64_t invalidId = std::numeric_limits<std::int64_t>::min();
    static constexpr std::size_t maximumSummaryBytes() noexcept
    {
        std::size_t result = 0;
        for (int level = 0; level < levelCount; ++level)
            result += (capacitySamples / (static_cast<std::size_t>(baseBinSamples) << level) + 3) * sizeof(Bin);
        return result;
    }
    void clear() noexcept
    {
        write = count = 0; end = 0; tail = {}; ++revision;
        for (auto& level : levels) for (auto& bin : level) bin.id = invalidId;
    }
    void append(const CapturePacket& packet, std::int64_t start)
    {
        allocate();
        jassert(count == 0 || start >= end);
        if (count != 0 && start < end) return;
        if (count == 0) end = start;
        if (start - end >= static_cast<std::int64_t>(capacitySamples))
        { clear(); end = start; }
        const float missing = std::numeric_limits<float>::quiet_NaN();
        while (end < start && end % baseBinSamples != 0) appendSample({ missing, missing });
        const auto missingBins = (start - end) / baseBinSamples;
        if (missingBins > 0)
        {
            const auto missingSamples = static_cast<std::size_t>(missingBins * baseBinSamples);
            const auto firstCount = std::min(missingSamples, capacitySamples - write);
            std::fill_n(storage.begin() + static_cast<std::ptrdiff_t>(write), firstCount, StereoSample { missing, missing });
            std::fill_n(storage.begin(), missingSamples - firstCount, StereoSample { missing, missing });
            const auto firstId = floorBin(end, baseBinSamples);
            for (std::int64_t i = 0; i < missingBins; ++i) publishBin(firstId + i, {});
            write = (write + missingSamples) % capacitySamples; count = std::min(count + missingSamples, capacitySamples);
            end += static_cast<std::int64_t>(missingSamples); tail = {};
        }
        while (end < start) appendSample({ missing, missing });
        for (int i = 0; i < packet.header.sampleCount; ++i)
            appendSample({ packet.left[static_cast<std::size_t>(i)], packet.right[static_cast<std::size_t>(i)] });
        ++revision;
    }
    std::size_t size() const noexcept { return count; }
    std::size_t allocatedBytes() const noexcept
    { return storage.capacity() * sizeof(StereoSample) + summaryBytes() + viewport.capacity() * sizeof(SampleExtrema); }
    std::size_t summaryBytes() const noexcept
    { std::size_t bytes = 0; for (const auto& level : levels) bytes += level.capacity() * sizeof(Bin); return bytes; }
    std::int64_t beginSample() const noexcept { return end - static_cast<std::int64_t>(count); }
    std::int64_t endSample() const noexcept { return end; }
    StereoSample at(std::int64_t sample) const noexcept
    {
        if (sample < beginSample() || sample >= end) return {};
        const auto oldest = (write + capacitySamples - count) % capacitySamples;
        return storage[(oldest + static_cast<std::size_t>(sample - beginSample())) % capacitySamples];
    }
    bool hasSample(std::int64_t sample) const noexcept
    { return sample >= beginSample() && sample < end && std::isfinite(at(sample).left); }
    static std::int64_t floorBin(std::int64_t sample, std::int64_t step) noexcept
    { const auto q = sample / step; return q - (sample % step < 0 ? 1 : 0); }
    SampleExtrema extrema(std::int64_t first, std::int64_t last) const noexcept
    {
        SampleExtrema result;
        first = std::max(first, beginSample()); last = std::min(last, end);
        while (first < last)
        {
            int selected = -1;
            for (int level = levelCount - 1; level >= 0; --level)
            {
                const auto step = static_cast<std::int64_t>(baseBinSamples) << level;
                if (first % step == 0 && last - first >= step && validBin(level, first / step))
                { selected = level; break; }
            }
            if (selected >= 0)
            {
                const auto step = static_cast<std::int64_t>(baseBinSamples) << selected;
                result.merge(bin(selected, first / step)); first += step;
            }
            else { const auto value = at(first++); if (std::isfinite(value.left)) result.add(value); }
        }
        return result;
    }
    // Partial newest/oldest bins query exact raw edges; stale ring slots are
    // never returned. Smaller-than-base views use raw sample geometry.
    SampleExtrema alignedBin(int level, std::int64_t id) const noexcept
    {
        const auto step = static_cast<std::int64_t>(baseBinSamples) << level;
        const auto first = id * step, last = first + step;
        if (first >= beginSample() && last <= end && validBin(level, id)) return bin(level, id);
        return extrema(first, last);
    }
    const std::vector<SampleExtrema>& bucketExtrema(double first, double last, int columns) const
    {
        if (columns <= 0 || columns > maxCachedColumns || ! std::isfinite(first) || ! std::isfinite(last) || last <= first)
        { viewport.clear(); viewportRevision = std::numeric_limits<std::uint64_t>::max(); return viewport; }
        if (viewportRevision == revision && juce::exactlyEqual(first, viewportFirst)
            && juce::exactlyEqual(last, viewportLast) && static_cast<int>(viewport.size()) == columns) return viewport;
        if (viewport.capacity() == 0) viewport.reserve(maxCachedColumns);
        viewport.resize(static_cast<std::size_t>(columns));
        const double step = (last - first) / columns;
        for (int x = 0; x < columns; ++x)
        {
            const auto a = static_cast<std::int64_t>(std::clamp(std::ceil(first + x * step), static_cast<double>(beginSample()), static_cast<double>(end)));
            const auto b = static_cast<std::int64_t>(std::clamp(std::ceil(x + 1 == columns ? last : first + (x + 1) * step), static_cast<double>(beginSample()), static_cast<double>(end)));
            viewport[static_cast<std::size_t>(x)] = extrema(a, b);
        }
        viewportFirst = first; viewportLast = last; viewportRevision = revision;
        return viewport;
    }
private:
    static std::size_t slot(std::int64_t id, std::size_t size) noexcept
    { auto r = id % static_cast<std::int64_t>(size); return static_cast<std::size_t>(r < 0 ? r + static_cast<std::int64_t>(size) : r); }
    bool validBin(int level, std::int64_t id) const noexcept
    { const auto& bins = levels[static_cast<std::size_t>(level)]; return ! bins.empty() && bins[slot(id, bins.size())].id == id; }
    SampleExtrema bin(int level, std::int64_t id) const noexcept
    { const auto& bins = levels[static_cast<std::size_t>(level)]; return bins[slot(id, bins.size())].value; }
    void allocate()
    {
        if (! storage.empty()) return;
        storage.resize(capacitySamples);
        for (int level = 0; level < levelCount; ++level)
            levels[static_cast<std::size_t>(level)].resize(capacitySamples / (static_cast<std::size_t>(baseBinSamples) << level) + 3);
    }
    void appendSample(StereoSample value)
    {
        if (end % baseBinSamples == 0) tail = {};
        storage[write] = value;
        if (std::isfinite(value.left)) tail.add(value);
        write = (write + 1) % capacitySamples; count = std::min(count + 1, capacitySamples); ++end;
        if (end % baseBinSamples != 0) return;
        publishBin(floorBin(end, baseBinSamples) - 1, tail);
    }
    void publishBin(std::int64_t id, SampleExtrema aggregate)
    {
        for (int level = 0; level < levelCount; ++level)
        {
            auto& bins = levels[static_cast<std::size_t>(level)];
            bins[slot(id, bins.size())] = { id, aggregate };
            if (id % 2 == 0 || ! validBin(level, id - 1)) break;
            auto left = bin(level, id - 1); left.merge(aggregate); aggregate = left;
            id = floorBin(id, 2);
        }
    }
    std::array<std::vector<Bin>, levelCount> levels;
    std::vector<StereoSample> storage;
    SampleExtrema tail;
    std::size_t write = 0, count = 0;
    std::int64_t end = 0;
    std::uint64_t revision = 0;
    mutable std::vector<SampleExtrema> viewport;
    mutable std::uint64_t viewportRevision = std::numeric_limits<std::uint64_t>::max();
    mutable double viewportFirst = 0, viewportLast = 0;
};
}
