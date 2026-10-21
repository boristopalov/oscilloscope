#include "WaveformSurface.h"
#include "WaveformShaders.h"
#include "ScopeLookAndFeel.h"
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CoreVideo.h>
#include <map>

@interface ScopeMetalView : NSView
@end
@implementation ScopeMetalView
- (BOOL)isOpaque { return NO; }
- (NSView*)hitTest:(NSPoint)point { (void)point; return nil; }
@end

namespace oscilloscope
{
namespace
{
struct FrameSubscription
{
    std::function<void(double)> callback;
    std::uint32_t display = 0;
};
// One display link / one main-thread delivery per screen. Every editor on that
// screen receives the same target before capture/UI timers can run in between.
struct DisplayDriver
{
    struct State
    {
        std::vector<std::weak_ptr<FrameSubscription>> subscriptions;
        std::atomic<bool> stopped { false }, pending { false };
        std::atomic<double> target { 0 };
        std::uint32_t display = 0;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    CVDisplayLinkRef link = nullptr;
    explicit DisplayDriver(std::uint32_t display)
    {
        state->display = display;
        if (CVDisplayLinkCreateWithCGDisplay(display, &link) != kCVReturnSuccess) return;
        CVDisplayLinkSetOutputCallback(link, [](CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp* output,
            CVOptionFlags, CVOptionFlags*, void* context) -> CVReturn
        {
            const auto shared = static_cast<DisplayDriver*>(context)->state;
            const double period = output->videoTimeScale > 0 && output->videoRefreshPeriod > 0
                ? static_cast<double>(output->videoRefreshPeriod) / output->videoTimeScale : 1.0 / 60.0;
            // Rendering against an imminent refresh raced the compositor and
            // alternated between two presentation phases. Give CPU/GPU one full
            // refresh of lead time, and evaluate the camera for that same target.
            shared->target.store(static_cast<double>(output->hostTime) / CVGetHostClockFrequency() + period);
            if (! shared->pending.exchange(true))
                dispatch_async(dispatch_get_main_queue(), ^{
                    shared->pending.store(false);
                    if (shared->stopped.load()) return;
                    const auto target = shared->target.load();
                    const auto subscribers = shared->subscriptions;
                    for (const auto& weak : subscribers)
                        if (auto subscription = weak.lock(); subscription && subscription->display == shared->display)
                            subscription->callback(target);
                });
            return kCVReturnSuccess;
        }, this);
        CVDisplayLinkStart(link);
    }
    ~DisplayDriver()
    {
        state->stopped = true;
        if (link) { CVDisplayLinkStop(link); CVDisplayLinkRelease(link); }
    }
};
struct DisplayDrivers
{
    std::map<std::uint32_t, std::unique_ptr<DisplayDriver>> drivers;
    DisplayDriver& get(std::uint32_t display)
    {
        auto& driver = drivers[display];
        if (! driver) driver = std::make_unique<DisplayDriver>(display);
        return *driver;
    }
};
}
struct FramePacer::Impl
{
    juce::SharedResourcePointer<DisplayDrivers> drivers;
    std::shared_ptr<FrameSubscription> subscription = std::make_shared<FrameSubscription>();
};
FramePacer::FramePacer(std::function<void(double)> callback) : impl(std::make_unique<Impl>())
{ impl->subscription->callback = std::move(callback); setDisplay(CGMainDisplayID()); }
FramePacer::~FramePacer() = default;
bool FramePacer::running() const noexcept
{
    const auto& driver = impl->drivers->get(impl->subscription->display);
    return driver.link && CVDisplayLinkIsRunning(driver.link);
}
void FramePacer::setDisplay(std::uint32_t id)
{
    if (id == 0 || id == impl->subscription->display) return;
    impl->subscription->display = id;
    auto& subscriptions = impl->drivers->get(id).state->subscriptions;
    subscriptions.erase(std::remove_if(subscriptions.begin(), subscriptions.end(), [&](const auto& weak)
        { const auto existing = weak.lock(); return ! existing || existing == impl->subscription; }), subscriptions.end());
    subscriptions.push_back(impl->subscription);
}

namespace
{
struct Column { float low[2], high[2]; std::uint32_t present; };
struct Point { float x, values[2]; };
struct Params
{
    std::uint32_t count, traces, overlay, raw;
    float width, height, scale, range, threshold, xStep, xOffset;
};
struct Colour { float r, g, b, a; };
struct Palette { Colour colour[2], interior, warning; };
Colour rgba(juce::Colour c)
{ const auto a = c.getFloatAlpha(); return { c.getFloatRed()*a, c.getFloatGreen()*a, c.getFloatBlue()*a, a }; }
struct Backend
{
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> commands = nil;
    id<MTLRenderPipelineState> wave = nil, raw = nil;
    int samples = 4;
    std::uint64_t generation = 0;
    bool usable = false;
    double retryAfter = 0;
    static constexpr double retrySeconds = 1;
    Backend() { prepare(); }
    bool ready() const noexcept { return usable; }
    void requestRecovery(std::uint64_t failedGeneration)
    {
        // A completion from an older backend must not invalidate its successor.
        if (! usable || failedGeneration != generation) return;
        usable = false;
        retryAfter = CACurrentMediaTime() + retrySeconds;
    }
    bool prepare()
    {
        if (ready()) return true;
        const auto now = CACurrentMediaTime();
        if (now < retryAfter) return false;
        retryAfter = now + retrySeconds;
        initialise();
        usable = device && commands && wave && raw;
        if (usable) ++generation;
        return usable;
    }
    void initialise()
    {
        commands = nil; wave = raw = nil;
        device = MTLCreateSystemDefaultDevice();
        if (! device) return;
        samples = [device supportsTextureSampleCount:4] ? 4 : 1;
        commands = [device newCommandQueue];
        if (! commands) return;
        NSError* error = nil;
        NSString* source = [NSString stringWithUTF8String:waveformMetalSource];
        if (! source) return;
        auto library = [device newLibraryWithSource:(NSString* _Nonnull)source options:nil error:&error];
        if (! library) { juce::Logger::writeToLog("Waveform Metal shader: " + juce::String([[error description] UTF8String])); return; }
        auto pipeline = [&](NSString* vertex, NSString* fragment)
        {
            auto descriptor = [MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction = [library newFunctionWithName:vertex];
            descriptor.fragmentFunction = [library newFunctionWithName:fragment];
            descriptor.rasterSampleCount = static_cast<NSUInteger>(samples);
            auto colour = descriptor.colorAttachments[0]; colour.pixelFormat = MTLPixelFormatBGRA8Unorm;
            colour.blendingEnabled = YES;
            colour.sourceRGBBlendFactor = colour.sourceAlphaBlendFactor = MTLBlendFactorOne;
            colour.destinationRGBBlendFactor = colour.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            return [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        };
        wave = pipeline(@"waveVertex", @"contourFragment"); raw = pipeline(@"rawVertex", @"rawFragment");
        if (! wave || ! raw) juce::Logger::writeToLog("Waveform Metal pipeline: " + juce::String([[error description] UTF8String]));
    }
};
struct Slot { id<MTLBuffer> buffer; std::atomic<bool> busy { false }; };
struct Failures
{
    std::atomic<std::uint64_t> count { 0 }, backendGeneration { 0 };
};
struct Record
{
    double target = 0, cpuMs = 0, cursor = 0, submittedAt = 0, preparationMs = 0;
    std::atomic<double> presented { 0 }, gpuMs { 0 };
    std::atomic<bool> completed { false };
};
}
struct WaveformSurface::Impl
{
    juce::SharedResourcePointer<Backend> backend;
    ScopeMetalView* view = nil;
    CAMetalLayer* layer = nil;
    id<MTLTexture> msaa = nil;
    std::array<std::shared_ptr<Slot>, 3> slots;
    std::shared_ptr<Failures> errors = std::make_shared<Failures>();
    std::vector<std::shared_ptr<Record>> records;
    std::uint64_t skipped = 0, handledErrors = 0, backendGeneration = 0;
    bool ready = false;
    double retryAfter = 0;
    int width = 0, height = 0;
    Impl()
    {
        for (auto& slot : slots) slot = std::make_shared<Slot>();
    }
};
WaveformSurface::WaveformSurface() : impl(std::make_unique<Impl>())
{
    prepare();
    setInterceptsMouseClicks(false, false);
}
WaveformSurface::~WaveformSurface() { setView(nullptr); }
bool WaveformSurface::prepare()
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    auto& state = *impl;
    const auto now = CACurrentMediaTime();
    const auto failures = state.errors->count.load(std::memory_order_acquire);
    if (failures != state.handledErrors)
    {
        state.handledErrors = failures;
        state.ready = false;
        state.retryAfter = now + Backend::retrySeconds;
        state.backend->requestRecovery(state.errors->backendGeneration.load(std::memory_order_acquire));
    }
    if (available()) return true;
    state.ready = false;
    if (now < state.retryAfter) return false;
    // Reuse one bounded ring. Never discard busy slots and allocate a new ring
    // while old commands might still be stalled or completing in the driver.
    for (const auto& slot : state.slots)
        if (slot->busy.load(std::memory_order_acquire)) return false;
    if (! state.backend->prepare())
    { state.retryAfter = now + Backend::retrySeconds; return false; }
    if (! state.view)
    {
        state.view = [[ScopeMetalView alloc] initWithFrame:NSZeroRect];
        state.view.wantsLayer = YES;
        setView((__bridge void*)state.view);
    }
    // Replace the layer to discard stale drawable contents during recovery.
    // Submitted commands retain their Metal resources independently of this view.
    state.layer = [CAMetalLayer layer]; state.layer.device = state.backend->device;
    state.layer.pixelFormat = MTLPixelFormatBGRA8Unorm; state.layer.framebufferOnly = YES;
    state.layer.opaque = NO; state.layer.backgroundColor = nil; state.layer.maximumDrawableCount = 3;
    state.layer.displaySyncEnabled = YES; state.layer.presentsWithTransaction = NO;
    state.layer.allowsNextDrawableTimeout = YES; state.view.layer = state.layer;
    state.msaa = nil; state.width = state.height = 0;
    for (const auto& slot : state.slots) slot->buffer = nil;
    state.backendGeneration = state.backend->generation;
    state.ready = state.view != nil && state.layer != nil;
    if (! state.ready) state.retryAfter = now + Backend::retrySeconds;
    return state.ready;
}
float WaveformSurface::pixelScale() const noexcept
{
    if (! impl->view.window || getWidth() <= 0) return 1;
    return static_cast<float>(impl->view.bounds.size.width * impl->view.window.backingScaleFactor / getWidth());
}
std::uint32_t WaveformSurface::displayId() const noexcept
{ return static_cast<std::uint32_t>([impl->view.window.screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]); }
bool WaveformSurface::available() const noexcept
{
    return impl->ready && impl->backend->ready()
        && impl->backendGeneration == impl->backend->generation
        && impl->handledErrors == impl->errors->count.load(std::memory_order_acquire);
}
std::uint64_t WaveformSurface::skippedFrames() const noexcept { return impl->skipped; }
std::uint64_t WaveformSurface::failedFrames() const noexcept { return impl->errors->count.load(); }
bool WaveformSurface::render(const std::vector<WaveformDraw>& draws, double target, double cursor, double preparationMs)
{
    @autoreleasepool
    {
        if (! available() || ! isShowing() || getWidth() <= 0 || getHeight() <= 0) return false;
        auto& state = *impl;
        // Deliver telemetry only on the message thread. Completion handlers own
        // slots/records, never an editor pointer; teardown needs no GPU wait.
        for (auto it = state.records.begin(); it != state.records.end();)
        {
            const auto& r = *it;
            if (r->completed.load() && (r->presented.load() > 0 || target - r->target > 1))
            {
                if (onPresented) onPresented({ r->target, r->presented.load(), r->cpuMs, r->gpuMs.load(), r->cursor, r->submittedAt, r->preparationMs });
                it = state.records.erase(it);
            }
            else ++it;
        }
        if (! state.view.window || (state.view.window.occlusionState & NSWindowOcclusionStateVisible) == 0)
        {
            // Negative CPU time denotes an explicitly occluded target, not a
            // failed drawable; optional telemetry must not bridge hidden time.
            if (onPresented) onPresented({ target, 0, -1, 0, cursor });
            return false;
        }
        std::shared_ptr<Slot> slot;
        for (const auto& candidate : state.slots)
            if (! candidate->busy.exchange(true)) { slot = candidate; break; }
        if (! slot) { ++state.skipped; return false; }
        const double began = CACurrentMediaTime();
        const auto scale = pixelScale();
        const int width = static_cast<int>(std::ceil(static_cast<float>(getWidth())*scale)), height = static_cast<int>(std::ceil(static_cast<float>(getHeight())*scale));
        if (static_cast<std::int64_t>(width) * height > 8 * 1024 * 1024)
        { ++state.errors->count; slot->busy = false; return false; }
        if (state.width != width || state.height != height)
        {
            state.layer.contentsScale = scale; state.layer.drawableSize = CGSizeMake(width, height);
            if (state.backend->samples > 1)
            {
                auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:static_cast<NSUInteger>(width) height:static_cast<NSUInteger>(height) mipmapped:NO];
                descriptor.textureType = MTLTextureType2DMultisample; descriptor.sampleCount = static_cast<NSUInteger>(state.backend->samples);
                descriptor.storageMode = MTLStorageModePrivate; descriptor.usage = MTLTextureUsageRenderTarget;
                state.msaa = [state.backend->device newTextureWithDescriptor:descriptor];
                if (! state.msaa) { ++state.errors->count; slot->busy = false; return false; }
            }
            state.width = width; state.height = height;
        }
        std::size_t bytes = 0;
        for (const auto& draw : draws)
            bytes += ((draw.frame.individualSamples ? draw.frame.points.size()*sizeof(Point) : draw.frame.buckets.size()*sizeof(Column)) + 255) & ~std::size_t{255};
        bytes = std::max<std::size_t>(256, bytes);
        // Bound each viewer's upload ring even for malformed/extreme dimensions.
        if (bytes > 8*1024*1024) { ++state.errors->count; slot->busy = false; return false; }
        if (! slot->buffer || slot->buffer.length < bytes)
            slot->buffer = [state.backend->device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if (! slot->buffer) { ++state.errors->count; slot->busy = false; return false; }
        id<CAMetalDrawable> drawable = [state.layer nextDrawable];
        if (! drawable) { ++state.skipped; slot->busy = false; return false; }
        auto command = [state.backend->commands commandBuffer];
        auto pass = [MTLRenderPassDescriptor renderPassDescriptor]; auto attachment = pass.colorAttachments[0];
        attachment.texture = state.backend->samples > 1 ? state.msaa : drawable.texture;
        attachment.resolveTexture = state.backend->samples > 1 ? drawable.texture : nil;
        attachment.loadAction = MTLLoadActionClear;
        attachment.storeAction = state.backend->samples > 1 ? MTLStoreActionMultisampleResolve : MTLStoreActionStore;
        attachment.clearColor = MTLClearColorMake(0,0,0,0);
        auto encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (! command || ! encoder)
        {
            state.errors->backendGeneration.store(state.backendGeneration, std::memory_order_release);
            ++state.errors->count; slot->busy = false; return false;
        }
        std::size_t offset = 0;
        for (const auto& draw : draws)
        {
            const auto& f = draw.frame;
            const auto count = f.individualSamples ? f.points.size() : f.buckets.size();
            if (count == 0 || draw.area.isEmpty()) continue;
            const auto area = draw.area * scale;
            const auto clipped = area.getIntersection(juce::Rectangle<float>(0, 0, static_cast<float>(width), static_cast<float>(height)));
            if (clipped.isEmpty()) continue;
            Params params { static_cast<std::uint32_t>(count), static_cast<std::uint32_t>(ChannelProjector::traceCount(f.mode)),
                draw.overlay ? 1u : 0u, f.individualSamples ? 1u : 0u,
                area.getWidth(), area.getHeight(), scale, draw.range, draw.threshold, f.xStep, f.xOffset };
            Palette palette { { rgba(draw.colour), rgba(draw.colour.brighter(0.25f)) },
                rgba(scopeStyle::plot.interpolatedWith(draw.colour.withAlpha(1.0f), 0.38f).withAlpha(draw.colour.getFloatAlpha())),
                rgba(juce::Colour(0xfff08065).withAlpha(draw.colour.getFloatAlpha())) };
            auto* destination = static_cast<char*>(slot->buffer.contents) + offset;
            std::size_t size = 0;
            if (f.individualSamples)
            {
                auto* points = reinterpret_cast<Point*>(destination);
                for (const auto& p : f.points) *points++ = { p.x, { p.values[0], p.values[1] } };
                size = count*sizeof(Point);
            }
            else
            {
                auto* columns = reinterpret_cast<Column*>(destination);
                for (const auto& b : f.buckets) *columns++ = { {b.low[0],b.low[1]}, {b.high[0],b.high[1]}, b.present ? 1u : 0u };
                size = count*sizeof(Column);
            }
            [encoder setViewport:MTLViewport { area.getX(), area.getY(), area.getWidth(), area.getHeight(), 0, 1 }];
            const auto left = static_cast<NSUInteger>(std::max(0.0f, std::floor(clipped.getX())));
            const auto top = static_cast<NSUInteger>(std::max(0.0f, std::floor(clipped.getY())));
            [encoder setScissorRect:MTLScissorRect { left, top,
                static_cast<NSUInteger>(std::min(static_cast<float>(width), std::ceil(clipped.getRight()))) - left,
                static_cast<NSUInteger>(std::min(static_cast<float>(height), std::ceil(clipped.getBottom()))) - top }];
            [encoder setRenderPipelineState:f.individualSamples ? state.backend->raw : state.backend->wave];
            [encoder setVertexBuffer:slot->buffer offset:offset atIndex:0];
            [encoder setVertexBytes:&params length:sizeof(params) atIndex:1];
            [encoder setVertexBytes:&palette length:sizeof(palette) atIndex:2];
            [encoder setFragmentBytes:&params length:sizeof(params) atIndex:1];
            [encoder setFragmentBytes:&palette length:sizeof(palette) atIndex:2];
            const float origin = area.getY(); [encoder setFragmentBytes:&origin length:sizeof(origin) atIndex:3];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4
                instanceCount:count*params.traces*(f.individualSamples ? 3 : draw.overlay ? 4 : 7)];
            offset += (size + 255) & ~std::size_t{255};
        }
        [encoder endEncoding];
        std::shared_ptr<Record> record;
        if (onPresented && state.records.size() < 256)
        {
            record = std::make_shared<Record>(); record->target = target; record->cursor = cursor; record->submittedAt = began; record->preparationMs = preparationMs; state.records.push_back(record);
            [drawable addPresentedHandler:^(id<MTLDrawable> d) { record->presented.store(d.presentedTime); }];
        }
        auto errors = state.errors;
        const auto backendGeneration = state.backendGeneration;
        [command addCompletedHandler:^(id<MTLCommandBuffer> completed)
        {
            if (completed.status != MTLCommandBufferStatusCompleted)
            {
                errors->backendGeneration.store(backendGeneration, std::memory_order_release);
                errors->count.fetch_add(1, std::memory_order_release);
            }
            if (record) { record->gpuMs.store((completed.GPUEndTime-completed.GPUStartTime)*1000); record->completed = true; }
            slot->busy.store(false);
        }];
        [command presentDrawable:(id<CAMetalDrawable> _Nonnull)drawable atTime:target]; [command commit];
        if (record) record->cpuMs = (CACurrentMediaTime()-began)*1000;
        return true;
    }
}
}
