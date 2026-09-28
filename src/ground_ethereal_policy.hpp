#pragma once
// Native item-data flag identified by D2R 93847 controlled samples:
// ethereal uar: 0x00C02010; nonethereal uar: 0x00800010. Two bits
// differed (0x00400000, 0x00002000); this mask is the established D2
// ethereal bit and is now tested via a guarded runtime read. Do NOT
// interpret the other differing bit as an ethereal bit.
#include <cstdint>
namespace SoE::LootFilter::GroundEthereal {
constexpr std::uint32_t NativeEtherealMask = 0x00400000U;
constexpr bool FromNativeFlags(std::uint32_t flags) noexcept {
    return (flags & NativeEtherealMask)!=0U;
}
} // namespace SoE::LootFilter::GroundEthereal
