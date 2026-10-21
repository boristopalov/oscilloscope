#pragma once
#include "ProbeData.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <limits>
#include <memory>
#include <new>

namespace oscilloscope
{
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
constexpr int captureChunkSamples = 2048;
constexpr int captureQueueSlots = 128; // AbstractFifo leaves one slot unused.

struct CaptureHeader
{
    std::uint64_t generation = 0, sequence = 0;
    std::int64_t samplePosition = 0;
    double ppq = 0, bpm = 0, barStartPpq = 0;
    double captureTime = 0; // Monotonic callback start, seconds; independent of host transport.
    double loopStartPpq = 0, loopEndPpq = 0;
    std::uint64_t hostTimeNs = 0;
    std::uint32_t valid = 0, nonFinite = 0;
    int sampleCount = 0;
    int numerator = 0, denominator = 0;
    bool playing = false, looping = false;
};
struct CapturePacket
{
    CaptureHeader header;
    std::array<float, captureChunkSamples> left {}, right {};
};

// One audio producer, one display consumer. Reserve a WHOLE callback and publish
// once, so a reader can never observe a partially copied callback. Neither side
// resets indices, even across capture sessions or prepare/release.
class CaptureQueue
{
public:
    enum class Storage { immediate, deferred };
    explicit CaptureQueue(Storage storage = Storage::immediate)
    {
        if (storage == Storage::immediate)
        {
            ownedPackets = std::make_unique<Packets>();
            packets.store(ownedPackets.get(), std::memory_order_release);
        }
    }
    // Control thread only, serialized by the registry mutex. Publish storage
    // once before admission; retain it until the whole SourceState is destroyed.
    // Neither producer nor consumer reads/mutates the owning pointer.
    bool prepareStorage() noexcept
    {
        if (packets.load(std::memory_order_acquire) != nullptr) return true;
        auto prepared = std::unique_ptr<Packets>(new (std::nothrow) Packets);
        if (! prepared) return false;
        ownedPackets = std::move(prepared);
        packets.store(ownedPackets.get(), std::memory_order_release);
        return true;
    }
    static constexpr std::size_t storageBytes() noexcept { return sizeof(CapturePacket) * captureQueueSlots; }
    std::size_t allocatedBytes() const noexcept
    { return packets.load(std::memory_order_acquire) != nullptr ? storageBytes() : 0; }
    template <typename Sample>
    bool push(const Sample* left, const Sample* right, int count, CaptureHeader header) noexcept
    {
        if (count <= 0) return true;
        auto* const storage = packets.load(std::memory_order_acquire);
        if (storage == nullptr) return false;
        const auto chunks = 1 + (count - 1) / captureChunkSamples;
        if (chunks > captureQueueSlots - 1) return false;
        int start1, size1, start2, size2;
        fifo.prepareToWrite(chunks, start1, size1, start2, size2);
        if (size1 + size2 != chunks) return false;
        for (int chunk = 0; chunk < chunks; ++chunk)
        {
            auto& packet = (*storage)[static_cast<std::size_t>(chunk < size1 ? start1 + chunk : start2 + chunk - size1)];
            const int offset = chunk * captureChunkSamples;
            packet.header = header;
            packet.header.sequence += static_cast<std::uint64_t>(chunk);
            packet.header.sampleCount = std::min(captureChunkSamples, count - offset);
            packet.header.nonFinite = 0;
            if (header.captureTime > 0) packet.header.captureTime += offset / supportedSampleRate;
            if ((header.valid & hasSamples) != 0) packet.header.samplePosition += offset;
            if ((header.valid & (hasPpq | hasBpm)) == (hasPpq | hasBpm))
                packet.header.ppq += offset * header.bpm / (60.0 * supportedSampleRate);
            else if (offset != 0)
                packet.header.valid &= ~hasPpq; // No tempo: cannot timestamp this chunk's PPQ.
            if (offset != 0 && header.looping && (header.valid & hasLoopBounds) != 0
                && header.loopEndPpq > header.loopStartPpq && packet.header.ppq >= header.loopEndPpq)
                packet.header.ppq -= std::floor((packet.header.ppq - header.loopStartPpq)
                    / (header.loopEndPpq - header.loopStartPpq)) * (header.loopEndPpq - header.loopStartPpq);
            if ((header.valid & hasHostTime) != 0)
                packet.header.hostTimeNs += static_cast<std::uint64_t>(offset * (1.0e9 / supportedSampleRate));
            for (int n = 0; n < packet.header.sampleCount; ++n)
            {
                auto convert = [&packet](Sample value)
                {
                    // Double values outside float range are also non-finite for display.
                    if (! std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                    { ++packet.header.nonFinite; return 0.0f; }
                    return static_cast<float>(value);
                };
                packet.left[static_cast<std::size_t>(n)] = convert(left[offset + n]);
                packet.right[static_cast<std::size_t>(n)] = convert(right[offset + n]);
            }
        }
        fifo.finishedWrite(chunks);
        return true;
    }
    // The packet reference is borrowed only for this call; release after use.
    template <typename Consumer>
    bool consume(Consumer&& consumer)
    {
        auto* const storage = packets.load(std::memory_order_acquire);
        if (storage == nullptr) return false;
        int start1, size1, start2, size2;
        fifo.prepareToRead(1, start1, size1, start2, size2);
        if (size1 + size2 == 0) return false;
        consumer((*storage)[static_cast<std::size_t>(size1 != 0 ? start1 : start2)]);
        fifo.finishedRead(1);
        return true;
    }
    int ready() const noexcept { return fifo.getNumReady(); }
private:
    using Packets = std::array<CapturePacket, captureQueueSlots>;
    static_assert(std::atomic<Packets*>::is_always_lock_free);
    juce::AbstractFifo fifo { captureQueueSlots };
    std::unique_ptr<Packets> ownedPackets;
    std::atomic<Packets*> packets { nullptr };
};
}
