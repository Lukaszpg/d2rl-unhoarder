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
// Measured in D2R 93847 / SoE 0.18.194, item Divine Orb, 0.2.2.
// Conservative first write-capable PoC, not an assumption for all locales.
inline bool VanillaGroundLabel(const std::array<float,4>& rgba) noexcept {
    for (unsigned i=0;i<3;++i)
        if (!std::isfinite(rgba[i]) || std::fabs(rgba[i]-0.941f)>0.03f)
            return false;
    return std::isfinite(rgba[3]) && rgba[3]>=0.99f && rgba[3]<=1.001f;
}
}
