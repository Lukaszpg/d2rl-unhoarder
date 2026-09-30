#pragma once
// D2R build 93847 native ItemData+0x18, qualified by the same item pair:
// unidentified unique Sacred Armor 0x10800000 -> SDK identified=0;
// identified unique Sacred Armor 0x00800010 -> SDK identified=1.
// Each matched in verified presentation mode 5 and ground mode 3.
// Unknown pointer/flags is NOT interpreted as unidentified.
#include <cstdint>
namespace UnHoarder::GroundIdentified {
constexpr std::uint32_t NativeIdentifiedMask=0x00000010U;
constexpr bool FromNativeFlags(std::uint32_t flags) noexcept {
    return (flags & NativeIdentifiedMask)!=0U;
}
} // namespace UnHoarder::GroundIdentified
