#pragma once
#include <array>
#include <cstdint>
#include <limits>

// Pure predicates for the first experimental hidden-hover color PoC. The
// actual hook requires additional live evidence before using these checks.
namespace NativeRowBgTrialPolicy {
constexpr bool SingleRow(std::uint64_t before,
    std::uint64_t after) noexcept {
    return before==0 && after==1;
}
constexpr bool VanillaHiddenBlack(
    const std::array<std::uint32_t,4>& bits) noexcept {
    return bits==std::array<std::uint32_t,4>{{
        0x00000000U,0x00000000U,0x00000000U,0x3F19999AU}};
}
// Conservative 2,000 microsecond window; is NOT proof of item identity.
constexpr bool RecentQpc(std::int64_t append,std::int64_t render,
    std::int64_t frequency) noexcept {
    return frequency>0 && append>0 && render>=append &&
        render-append<=frequency/500; // <= 2 ms
}
}
