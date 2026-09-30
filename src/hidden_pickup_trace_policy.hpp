#pragma once
#include <cstdint>
namespace UnHoarder::HiddenPickupTracePolicy {
// A candidate is never equivalent to an actual native D2R click target.
constexpr bool FreshHint(std::uint64_t now,std::uint64_t observed,
                         std::uint64_t maxAge) noexcept {
    return observed!=0 && now>=observed && now-observed<=maxAge;
}
constexpr bool InCaptureWindow(std::uint64_t now,std::uint64_t click,
                               std::uint64_t duration) noexcept {
    return click!=0 && now>=click && now-click<=duration;
}
constexpr bool CaptureFinished(std::uint64_t now,std::uint64_t click,
                               std::uint64_t deadline) noexcept {
    return click!=0 && now>=click && now-click>deadline;
}
}
