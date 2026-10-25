#pragma once
#include <array>
#include <bit>
#include <cstdint>
namespace UnHoarder::GroundLabelBackground {
// Invoked only after exact native caller, pointer pairing, copied color,
// current item identity, matching rule and exact label-name checks succeed.
inline bool KnownNativeBlack(const std::array<float,4>& color) noexcept {
    const auto bits=std::bit_cast<std::array<std::uint32_t,4>>(color);
    // Alpha is a replaceable native presentation value, not a build witness.
    // Every nonnegative IEEE-754 float in [0,1] has an unsigned representation
    // at most 0x3f800000. Reject NaN, infinities and out-of-domain values;
    // accept signed zero too. RGB remains exact black: selected/highlighted,
    // already styled and foreign non-black rows are never recolored here.
    const bool validAlpha=bits[3]<=0x3f800000U || bits[3]==0x80000000U;
    return bits[0]==0 && bits[1]==0 && bits[2]==0 && validAlpha;
}
}
