#include "ProbeService.h"
#include <algorithm>
#include <unistd.h>

namespace oscilloscope
{
ProbeService& ProbeService::instance()
{
    static ProbeService service;
    return service;
}

std::shared_ptr<ProbeSource> ProbeService::registerSource()
{
    std::lock_guard<std::mutex> lock(registryMutex);
    auto source = std::make_shared<ProbeSource>(nextId++);
    registeredSources.push_back(source);
    return source;
}

void ProbeService::unregisterSource(const std::shared_ptr<ProbeSource>& source)
{
    std::lock_guard<std::mutex> lock(registryMutex);
    registeredSources.erase(std::remove(registeredSources.begin(), registeredSources.end(), source),
                            registeredSources.end());
}

std::vector<std::shared_ptr<ProbeSource>> ProbeService::sources() const
{
    std::lock_guard<std::mutex> lock(registryMutex);
    return registeredSources;
}

void ProbeService::openViewer()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (viewers++ == 0)
    {
        restart();
    }
}

void ProbeService::closeViewer()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    jassert(viewers > 0);
    if (--viewers == 0)
    {
        pause();
        drain();
    }
}

void ProbeService::restart()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto old = captureState.load(std::memory_order_relaxed);
    for (const auto& source : sources())
    {
        source->observations.clear();
        source->continuity = {};
        source->evicted = 0;
        source->droppedAtSessionStart = source->dropped.load(std::memory_order_relaxed);
        // Do not reset queue indices or the producer's cumulative loss counter.
    }
    captureState.store((((old >> 1) + 1) << 1) | 1u, std::memory_order_release);
}

void ProbeService::pause()
{
    captureState.fetch_and(~std::uint64_t { 1 }, std::memory_order_release);
}

void ProbeService::drain()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto currentSources = sources(); // Strong handles survive concurrent teardown.
    if (currentSources.empty()) return;
    const auto generation = captureState.load(std::memory_order_acquire) >> 1;
    for (std::size_t n = 0; n < currentSources.size(); ++n)
    {
        auto& source = *currentSources[(roundRobin + n) % currentSources.size()];
        ProbeRecord record;
        for (int budget = 0; budget < 512 && source.queue.pop(record); ++budget)
        {
            if (record.generation != generation) continue;
            if (source.observations.size() == maxRetainedRecords)
            {
                source.observations.pop_front();
                ++source.evicted;
            }
            source.observations.push_back(source.continuity.observe(record));
        }
    }
    roundRobin = (roundRobin + 1) % currentSources.size();
}

bool ProbeService::exportCsv(const juce::File& destination)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    // Opening/writing a file occurs only here, never in processBlock.
    juce::TemporaryFile temporary(destination);
    {
        juce::FileOutputStream output(temporary.getFile());
        if (! output.openedOk()) return false;
        output << "schema,process_id,registry_token,source_id,generation,sequence,sample_rate,block_size,"
                  "valid_mask,sample_position,ppq,bpm,bar_start_ppq,seconds,meter_numerator,meter_denominator,"
                  "playing,looping,recording,double_precision,peak_l,peak_r,peak_offset_l,peak_offset_r,"
                  "non_finite,event,sample_error,ppq_error_samples,dropped_total,dropped_session,evicted_total\n";
        for (const auto& source : sources())
        {
            const auto dropped = source->dropped.load(std::memory_order_relaxed);
            for (const auto& observation : source->observations)
            {
                const auto& r = observation.record;
                auto field = [&output](bool present, double value)
                {
                    if (present) output << juce::String(value, 17);
                    output << ",";
                };
                output << "1," << static_cast<int>(getpid()) << "," << token << ","
                       << juce::String(source->id) << "," << juce::String(r.generation) << ","
                       << juce::String(r.sequence) << ",48000," << r.blockSize << ","
                       << static_cast<int>(r.valid) << ",";
                if ((r.valid & hasSamples) != 0) output << juce::String(r.samplePosition);
                output << ",";
                field((r.valid & hasPpq) != 0, r.ppq);
                field((r.valid & hasBpm) != 0, r.bpm);
                field((r.valid & hasBarStart) != 0, r.barStartPpq);
                field((r.valid & hasSeconds) != 0, r.seconds);
                if ((r.valid & hasMeter) != 0) output << r.numerator;
                output << ",";
                if ((r.valid & hasMeter) != 0) output << r.denominator;
                output << "," << static_cast<int>(r.playing) << "," << static_cast<int>(r.looping)
                       << "," << static_cast<int>(r.recording) << "," << static_cast<int>(r.doublePrecision) << ",";
                field(true, r.peakL);
                field(true, r.peakR);
                output << r.peakOffsetL << "," << r.peakOffsetR << "," << static_cast<int>(r.nonFinite)
                       << "," << eventName(observation.event) << ",";
                field(observation.hasSampleError, observation.sampleError);
                field(observation.hasPpqError, observation.ppqErrorInSamples);
                output << juce::String(dropped) << "," << juce::String(dropped - source->droppedAtSessionStart) << ","
                       << juce::String(source->evicted) << "\n";
            }
        }
        output.flush();
        if (output.getStatus().failed()) return false;
    }
    // Close the stream before the atomic replacement (TemporaryFile handles cleanup).
    return temporary.overwriteTargetFileWithTemporary();
}
}

#if OSCILLOSCOPE_DEV_PROBES
// Development-only hooks let the host test inspect the registry inside the loaded VST3.
// These perform registry work off the audio thread and are not plugin-format APIs.
extern "C" __attribute__((visibility("default"))) unsigned int oscilloscopeProbeSourceCount()
{
    return static_cast<unsigned int>(oscilloscope::ProbeService::instance().sources().size());
}
extern "C" __attribute__((visibility("default"))) const char* oscilloscopeProbeRegistryToken()
{
    return oscilloscope::ProbeService::instance().registryToken().toRawUTF8();
}
#endif
