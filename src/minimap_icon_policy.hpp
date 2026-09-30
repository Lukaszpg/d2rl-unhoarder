#pragma once

#include <cstdint>

namespace UnHoarder::MinimapIconPolicy {

inline constexpr std::int64_t MinimumSizePx = 12;
inline constexpr std::int64_t MaximumSizePx = 40;
inline constexpr float DefaultSizePx = 12.0F;

// JSON sizes are integer pixels. Values outside the supported range are
// clamped so the rendered marker is always between 12 and 40 pixels.
[[nodiscard]] constexpr bool TryNormalizeSizePx(
        std::int64_t requested,
        float& normalized) noexcept {
    const auto bounded = requested < MinimumSizePx
        ? MinimumSizePx
        : (requested > MaximumSizePx ? MaximumSizePx : requested);
    normalized = static_cast<float>(bounded);
    return true;
}

} // namespace UnHoarder::MinimapIconPolicy
