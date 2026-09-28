#pragma once
#include <cstdint>

// Pure temporal classification: no item ownership or rendering inference.
// A matching hidden-hover label heartbeat is necessary to start collection.
namespace HoverDiffPolicy {
enum class Phase : std::uint8_t { None, Hover, Transition, Away };
inline constexpr std::uint64_t RecentMs = 300;
inline constexpr std::uint64_t AwayMs = 1250;
inline constexpr std::uint64_t MaximumMs = 55000;
inline constexpr std::uint64_t RequiredHoverMs = 7500;
inline constexpr std::uint64_t RequiredAwayMs = 5000;

constexpr Phase Classify(std::uint64_t now, std::uint64_t started,
    std::uint64_t lastMatched, bool completed) noexcept {
    if (!started || now<started || now-started>MaximumMs ||
        completed || lastMatched>now || lastMatched<started)
        return Phase::None;
    const auto age=now-lastMatched;
    if (age<=RecentMs) return Phase::Hover;
    if (age>=AwayMs) return Phase::Away;
    return Phase::Transition;
}
constexpr bool HasBothWindows(std::uint64_t hoverMs,
    std::uint64_t awayMs) noexcept {
    return hoverMs>=RequiredHoverMs && awayMs>=RequiredAwayMs;
}
}
