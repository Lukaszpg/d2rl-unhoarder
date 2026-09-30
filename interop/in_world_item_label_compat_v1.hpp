#pragma once

#include <D2RLPlugin/context.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace D2RLInterop::InWorldItemLabelCompatV1 {

inline constexpr char ServiceName[] = "in-world-item-label-compat";
inline constexpr std::uint32_t ServiceVersion = 1;
inline constexpr std::uint64_t SupportedEntryRva = 0x000C0420ULL;

struct Event {
    std::uint32_t structSize;
    std::uint32_t flags;
    std::int32_t unitType;
    std::uint32_t classId;
    std::uint32_t unitId;
    std::uint32_t sourceLength;
    const void* nativeUnit;
    const char* source;
};

struct ActiveItem {
    std::uint32_t structSize;
    std::int32_t unitType;
    std::uint32_t classId;
    std::uint32_t unitId;
    const void* nativeUnit;
};

using ObserverFn = void(__cdecl*)(const Event*, void* userData) noexcept;
using TransformerFn = bool(__cdecl*)(
    const Event*, char* replacement, std::uint32_t capacity, void* userData) noexcept;
using IsReadyFn = bool(__cdecl*)() noexcept;
using RegisterObserverFn = bool(__cdecl*)(
    const D2RL::PluginContext*, ObserverFn, void*) noexcept;
using UnregisterObserverFn = bool(__cdecl*)(
    const D2RL::PluginContext*, ObserverFn, void*) noexcept;
using RegisterTransformerFn = bool(__cdecl*)(
    const D2RL::PluginContext*, TransformerFn, void*) noexcept;
using UnregisterTransformerFn = bool(__cdecl*)(
    const D2RL::PluginContext*, TransformerFn, void*) noexcept;
using GetCurrentItemFn = bool(__cdecl*)(ActiveItem*) noexcept;

struct Service {
    std::uint32_t serviceSize;
    std::uint32_t serviceVersion;
    std::uint64_t entryRva;
    IsReadyFn isReady;
    RegisterObserverFn registerObserver;
    UnregisterObserverFn unregisterObserver;
    RegisterTransformerFn registerTransformer;
    UnregisterTransformerFn unregisterTransformer;
    GetCurrentItemFn getCurrentItem;
};

inline constexpr std::uint32_t ServiceSize =
    static_cast<std::uint32_t>(sizeof(Service));
inline constexpr std::uint32_t ServiceRequiredSize = ServiceSize;

inline bool HasService(const Service* service) noexcept {
    return service != nullptr
        && service->serviceVersion >= ServiceVersion
        && service->serviceSize >= ServiceRequiredSize
        && service->entryRva == SupportedEntryRva
        && service->isReady
        && service->registerObserver
        && service->unregisterObserver
        && service->registerTransformer
        && service->unregisterTransformer
        && service->getCurrentItem;
}

static_assert(std::is_standard_layout_v<Event>);
static_assert(std::is_trivially_copyable_v<Event>);
static_assert(std::is_standard_layout_v<ActiveItem>);
static_assert(std::is_trivially_copyable_v<ActiveItem>);
static_assert(std::is_standard_layout_v<Service>);
static_assert(std::is_trivially_copyable_v<Service>);
static_assert(sizeof(Event) == 40);
static_assert(sizeof(ActiveItem) == 24);
static_assert(sizeof(Service) == 64);

} // namespace D2RLInterop::InWorldItemLabelCompatV1
