#pragma once

#include <D2RLPlugin/context.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace UnHoarder::TooltipCompatV1 {

// Small, opt-in compatibility contract for plugins that share UnHoarder's
// build-93847 tooltip hooks. It is intentionally not a generic hook manager.
inline constexpr char ServiceName[] = "unhoarder-tooltip-compat";
inline constexpr std::uint32_t ServiceVersion = 1;
inline constexpr char SupportedBuildName[] = "93847";

inline constexpr std::uint64_t SharedLabelPaintRva = 0x001FA8E0ULL;
inline constexpr std::uint64_t GlyphRendererRva    = 0x00658510ULL;

using RegistrationHandle = std::uint64_t;
inline constexpr RegistrationHandle InvalidRegistrationHandle = 0;
inline constexpr std::uint32_t MaxSubscribers = 16;

enum class Result : std::uint32_t {
    Success          = 0,
    InvalidArgument  = 1,
    Unsupported      = 2,
    NotHost          = 3,
    AlreadyRegistered= 4,
    NotFound         = 5,
    LimitExceeded    = 6,
    OwnerMismatch    = 7,
};

// Middleware must call next at most once. Calling it zero times suppresses the
// remainder of that render call. The raw arguments and caller return address
// are borrowed for this synchronous invocation only.
using SharedLabelPaintNextFn = void(__cdecl*)(
    void* nextContext,
    void* rect,
    void* textArg,
    void* colorArg) noexcept;

using SharedLabelPaintMiddlewareFn = void(__cdecl*)(
    void* userData,
    std::uintptr_t callerReturnAddress,
    void* rect,
    void* textArg,
    void* colorArg,
    SharedLabelPaintNextFn next,
    void* nextContext) noexcept;

using GlyphRendererNextFn = std::uint64_t(__cdecl*)(
    void* nextContext,
    void* glyphContext,
    float x,
    float y,
    const float* rgba) noexcept;

using GlyphRendererMiddlewareFn = std::uint64_t(__cdecl*)(
    void* userData,
    std::uintptr_t callerReturnAddress,
    void* glyphContext,
    float x,
    float y,
    const float* rgba,
    GlyphRendererNextFn next,
    void* nextContext) noexcept;

struct SharedLabelPaintRegistration {
    std::uint32_t structSize;
    std::uint32_t flags;
    SharedLabelPaintMiddlewareFn callback;
    void* userData;
};

struct GlyphRendererRegistration {
    std::uint32_t structSize;
    std::uint32_t flags;
    GlyphRendererMiddlewareFn callback;
    void* userData;
};

inline constexpr std::uint32_t SharedLabelPaintRegistrationSize =
    static_cast<std::uint32_t>(sizeof(SharedLabelPaintRegistration));
inline constexpr std::uint32_t GlyphRendererRegistrationSize =
    static_cast<std::uint32_t>(sizeof(GlyphRendererRegistration));

using RegisterSharedLabelPaintFn = Result(__cdecl*)(
    const D2RL::PluginContext* consumer,
    const SharedLabelPaintRegistration* registration,
    RegistrationHandle* handle) noexcept;

// A successful unregister is a quiescence barrier: after it returns, this
// registration is not executing and cannot begin another invocation. A host
// must not return Success while an old dispatch snapshot can still call the
// consumer. Unregistering a registration from that registration's own active
// callback is unsupported; do it after the callback returns.
using UnregisterSharedLabelPaintFn = Result(__cdecl*)(
    const D2RL::PluginContext* consumer,
    RegistrationHandle handle) noexcept;

using RegisterGlyphRendererFn = Result(__cdecl*)(
    const D2RL::PluginContext* consumer,
    const GlyphRendererRegistration* registration,
    RegistrationHandle* handle) noexcept;

// Same quiescence guarantee as unregisterSharedLabelPaint.
using UnregisterGlyphRendererFn = Result(__cdecl*)(
    const D2RL::PluginContext* consumer,
    RegistrationHandle handle) noexcept;

struct Service {
    std::uint32_t serviceSize;
    std::uint32_t serviceVersion;
    RegisterSharedLabelPaintFn registerSharedLabelPaint;
    UnregisterSharedLabelPaintFn unregisterSharedLabelPaint;
    RegisterGlyphRendererFn registerGlyphRenderer;
    UnregisterGlyphRendererFn unregisterGlyphRenderer;
};

inline constexpr std::uint32_t ServiceSize =
    static_cast<std::uint32_t>(sizeof(Service));
inline constexpr std::uint32_t ServiceRequiredSize = ServiceSize;

inline bool HasService(const Service* service) noexcept {
    return service != nullptr &&
        service->serviceVersion >= ServiceVersion &&
        service->serviceSize >= ServiceRequiredSize &&
        service->registerSharedLabelPaint != nullptr &&
        service->unregisterSharedLabelPaint != nullptr &&
        service->registerGlyphRenderer != nullptr &&
        service->unregisterGlyphRenderer != nullptr;
}

static_assert(std::is_standard_layout_v<SharedLabelPaintRegistration>);
static_assert(std::is_trivially_copyable_v<SharedLabelPaintRegistration>);
static_assert(std::is_standard_layout_v<GlyphRendererRegistration>);
static_assert(std::is_trivially_copyable_v<GlyphRendererRegistration>);
static_assert(std::is_standard_layout_v<Service>);
static_assert(std::is_trivially_copyable_v<Service>);
static_assert(sizeof(SharedLabelPaintRegistration) == 24);
static_assert(sizeof(GlyphRendererRegistration) == 24);
static_assert(sizeof(Service) == 40);

} // namespace UnHoarder::TooltipCompatV1
