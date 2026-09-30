#pragma once
// Production policy for the 93847 native one-hop reader. These are *only*
// quality/item-level and optional guarded ethereal/identified scalars; sockets use stat 194.
// This header is portable so the fail-open rule gate has real C++ tests.
#include <cstdint>
namespace UnHoarder::GroundPropertyLive {
// D2R build 93847: observed in native ground-label presentation as mode 5
// for 667 ms before the same item moved to mode 3. The mode-5 exception is
// limited to trusted label presentation; never use it to authorize pickup.
enum class Purpose : std::uint8_t { StrictGround, VerifiedLabel };
constexpr std::uint32_t GroundMode=3U, PresentingMode=5U;
constexpr bool AllowsMode(std::uint32_t mode,Purpose purpose) noexcept {
    return mode==GroundMode ||
        (purpose==Purpose::VerifiedLabel && mode==PresentingMode);
}
struct Scalars {
    bool qualityKnown{}, itemLevelKnown{};
    std::uint32_t quality{}, itemLevel{};
    bool etherealKnown{}, ethereal{};
    bool identifiedKnown{}, identified{};
};
constexpr Scalars Validate(std::uint32_t rawQuality,
    std::uint32_t rawLevel) noexcept {
    const bool q=rawQuality>=1U && rawQuality<=9U;
    const bool l=rawLevel>=1U && rawLevel<=99U;
    return {q,l,q?rawQuality:0U,l?rawLevel:0U};
}
} // namespace UnHoarder::GroundPropertyLive
