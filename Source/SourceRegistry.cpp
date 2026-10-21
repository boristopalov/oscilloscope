#include "SourceRegistry.h"
#include <algorithm>

namespace oscilloscope
{
const char* captureStatusText(CaptureStatus status) noexcept
{
    switch (status)
    {
        case CaptureStatus::waiting: return "Waiting for capture";
        case CaptureStatus::live: return "Live";
        case CaptureStatus::syncUnavailable: return "Sync unavailable - independent time view";
        case CaptureStatus::captureGap: return "Capture gap - Reset to resume";
        case CaptureStatus::unsupportedRate: return "Visualization requires 48 kHz - Reset after restoring rate";
        case CaptureStatus::stale: return "No recent data - Reset to rejoin";
        case CaptureStatus::incompatible: return "Incompatible timing - Reset to rejoin";
    }
    return "Waiting";
}
SourceRegistry& SourceRegistry::instance() { static SourceRegistry registry; return registry; }
std::shared_ptr<SourceState> SourceRegistry::registerSource()
{
    std::lock_guard<std::mutex> lock(mutex);
    auto source = std::make_shared<SourceState>(nextId++);
    registered.push_back(source);
    updateAdmission();
    return source;
}
void SourceRegistry::unregisterSource(const std::shared_ptr<SourceState>& source)
{
    std::lock_guard<std::mutex> lock(mutex);
    source->admitted.store(false, std::memory_order_release);
    registered.erase(std::remove(registered.begin(), registered.end(), source), registered.end());
    updateAdmission();
}
void SourceRegistry::updateAdmission()
{
    // Registration appends and removal preserves order: registered sources can
    // only move towards admission. Allocate once off audio before publishing
    // admission, and keep the queue owned by that source through removal.
    // Allocation failure leaves a source waiting and never throws in teardown.
    for (std::size_t n = 0; n < registered.size(); ++n)
        registered[n]->admitted.store(n < sourceLimit && registered[n]->queue.prepareStorage(), std::memory_order_release);
}
std::vector<std::shared_ptr<SourceState>> SourceRegistry::sources() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return registered; // Strong handles keep queues alive through concurrent removal.
}
}
