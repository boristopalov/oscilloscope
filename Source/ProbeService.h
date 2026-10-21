#pragma once
#include "ProbeData.h"
#include <juce_events/juce_events.h>
#include <juce_core/juce_core.h>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace oscilloscope
{
struct ProbeSource
{
    explicit ProbeSource(std::uint64_t sourceId) : id(sourceId) {}
    const std::uint64_t id;
    SpscQueue<ProbeRecord, 4096> queue;
    std::atomic<double> sampleRate { 0 };
    std::atomic<std::uint64_t> dropped { 0 }, processedBlocks { 0 };
    // All fields below belong exclusively to the message-thread consumer.
    std::deque<Observation> observations;
    ContinuityProbe continuity;
    std::uint64_t evicted = 0, droppedAtSessionStart = 0;
};

class ProbeService final
{
public:
    static ProbeService& instance();
    std::shared_ptr<ProbeSource> registerSource();
    void unregisterSource(const std::shared_ptr<ProbeSource>&);
    std::vector<std::shared_ptr<ProbeSource>> sources() const;
    void openViewer();
    void closeViewer();
    void restart();
    void pause();
    bool isRecording() const noexcept { return (captureState.load(std::memory_order_acquire) & 1u) != 0; }
    void drain(); // Sole consumer; message thread only, also usable by the test harness.
    bool exportCsv(const juce::File&);
    const juce::String& registryToken() const noexcept { return token; }

    // One coherent word: bit zero enables recording; remaining bits identify the session.
    // Processors retain a reference at construction, never find this service in a callback.
    std::atomic<std::uint64_t> captureState { 0 };
    static constexpr std::size_t maxRetainedRecords = 16384;
private:
    ProbeService() = default;
    mutable std::mutex registryMutex;
    std::vector<std::shared_ptr<ProbeSource>> registeredSources;
    std::uint64_t nextId = 1;
    int viewers = 0;
    std::size_t roundRobin = 0;
    const juce::String token = juce::Uuid().toString();
};
}

#if OSCILLOSCOPE_DEV_PROBES
extern "C" __attribute__((visibility("default"))) unsigned int oscilloscopeProbeSourceCount();
extern "C" __attribute__((visibility("default"))) const char* oscilloscopeProbeRegistryToken();
#endif
