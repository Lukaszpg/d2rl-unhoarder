#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace D2RLInterop::UnitStatReadCompatV1 {

// Cooperative ABI for the active owner of D2R's total UnitStat bridge.
// Service discovery remains provider-local: a consumer first identifies the
// plugin that owns the live bridge target, then acquires this service from that
// exact provider. No product-specific plugin ID is part of the contract.
inline constexpr char ServiceName[] = "unit-stat-read-compat";
inline constexpr std::uint32_t ServiceVersion = 1;
inline constexpr std::uint64_t SupportedEntryRva = 0x002F5020ULL;

using ReadStatFn = std::int32_t(__cdecl*)(
    void* borrowedUnit,
    std::int32_t statId,
    std::uint16_t layer) noexcept;

struct Service {
    std::uint32_t serviceSize;
    std::uint32_t serviceVersion;
    std::uint64_t entryRva;
    std::uintptr_t ownerTarget;
    ReadStatFn readStat;
};

inline constexpr std::uint32_t ServiceSize =
    static_cast<std::uint32_t>(sizeof(Service));
inline constexpr std::uint32_t ServiceRequiredSize = ServiceSize;

inline bool HasService(const Service* service) noexcept {
    return service != nullptr
        && service->serviceVersion >= ServiceVersion
        && service->serviceSize >= ServiceRequiredSize
        && service->entryRva == SupportedEntryRva
        && service->ownerTarget != 0
        && service->readStat != nullptr;
}

static_assert(std::is_standard_layout_v<Service>);
static_assert(std::is_trivially_copyable_v<Service>);
static_assert(sizeof(Service) == 32);

} // namespace D2RLInterop::UnitStatReadCompatV1
