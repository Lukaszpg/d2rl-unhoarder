#pragma once

#include <cstdint>
#include <limits>

namespace UnHoarder::AutomapProjectionProbe {

struct Point final {
    std::int32_t x{};
    std::int32_t y{};
};

struct ClipRect final {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t width{};
    std::int32_t height{};
};

// D2R client/dimetric coordinates are the native automap input coordinate
// system used by build 93847. MapSense independently qualified this exact
// world-subtile transform before passing points to D2R+0xD4910.
[[nodiscard]] constexpr auto WorldSubtileToClient(
        std::int32_t worldX,
        std::int32_t worldY,
        Point& output) noexcept -> bool {
    const auto x = std::int64_t{16}
        * (static_cast<std::int64_t>(worldX) - worldY);
    const auto y = std::int64_t{8}
        * (static_cast<std::int64_t>(worldX) + worldY);
    constexpr auto minimum = static_cast<std::int64_t>(
        (std::numeric_limits<std::int32_t>::min)());
    constexpr auto maximum = static_cast<std::int64_t>(
        (std::numeric_limits<std::int32_t>::max)());
    if (x < minimum || x > maximum || y < minimum || y > maximum) {
        output = {};
        return false;
    }
    output = {
        .x = static_cast<std::int32_t>(x),
        .y = static_cast<std::int32_t>(y),
    };
    return true;
}

[[nodiscard]] constexpr auto PackClientCoordinates(Point point) noexcept
        -> std::uint64_t {
    return static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(point.x))
        | (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(point.y)) << 32U);
}

[[nodiscard]] constexpr auto PlausibleClip(ClipRect clip) noexcept -> bool {
    return clip.width > 0 && clip.height > 0
        && clip.width <= 32'768 && clip.height <= 32'768
        && clip.left >= -32'768 && clip.left <= 32'768
        && clip.top >= -32'768 && clip.top <= 32'768;
}

[[nodiscard]] constexpr auto Contains(
        ClipRect clip,
        Point point) noexcept -> bool {
    if (!PlausibleClip(clip)) return false;
    const auto right = static_cast<std::int64_t>(clip.left) + clip.width;
    const auto bottom = static_cast<std::int64_t>(clip.top) + clip.height;
    return point.x >= clip.left && point.y >= clip.top
        && static_cast<std::int64_t>(point.x) < right
        && static_cast<std::int64_t>(point.y) < bottom;
}

} // namespace UnHoarder::AutomapProjectionProbe
