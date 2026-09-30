#pragma once
#include <array>
#include <cmath>
namespace NativeRowFontColorPolicy {
inline bool ValidJsonColor(const std::array<float,4>& rgba) noexcept {
    for (const float value:rgba)
        if (!std::isfinite(value) || value<0.f || value>1.f)
            return false;
    return true;
}
// The caller/row/item identity is qualified before this predicate runs.
// Do not require one historically observed vanilla RGB here: D2R may feed
// exact white or rarity-colored RGB for the same legitimate ground-label
// glyph. The remaining guard is strictly structural: finite normalized RGB
// and an opaque/near-opaque label glyph. Preserve the native alpha when
// forwarding the configured JSON RGB.
inline bool EligibleGroundLabel(const std::array<float,4>& rgba) noexcept {
    for (unsigned i=0;i<3;++i)
        if (!std::isfinite(rgba[i]) || rgba[i]<0.f || rgba[i]>1.001f)
            return false;
    return std::isfinite(rgba[3]) && rgba[3]>=0.90f && rgba[3]<=1.001f;
}
}
