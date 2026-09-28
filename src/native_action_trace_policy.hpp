#pragma once
#include <cstdint>

namespace SoE::LootFilter::NativeActionTracePolicy {

// D2R build 93847, recovered from native C at FBEF0 and F9BC0.
// These are observation-only offsets. NEVER write these fields.
inline constexpr std::uintptr_t PlayerInteractionDataOffset = 0x10;
inline constexpr std::uintptr_t PendingActionFieldsOffset = 0x1C8;
inline constexpr std::uint32_t ItemUnitType = 4;
inline constexpr std::uint64_t PhaseDurationMs = 7000;
inline constexpr std::uint64_t ClickNearbyMs = 1750;
inline constexpr std::uint64_t ClickPriorSamplingSkewMs = 200;

struct Pending final {
    std::uint32_t flag{},action{},targetType{},targetId{};
};
static_assert(sizeof(Pending) == 16);

constexpr bool IsPendingItem(const Pending& pending) noexcept {
    return pending.flag != 0 && pending.targetType == ItemUnitType &&
        pending.targetId != 0;
}
constexpr bool IsItemInteraction(std::uint32_t type,std::uint32_t id) noexcept {
    return type == ItemUnitType && id != 0;
}
constexpr bool NearClick(std::uint64_t eventMs,std::uint64_t clickMs) noexcept {
    return clickMs != 0 && ((eventMs >= clickMs && eventMs-clickMs <= ClickNearbyMs) ||
        (eventMs < clickMs && clickMs-eventMs <= ClickPriorSamplingSkewMs));
}
}
