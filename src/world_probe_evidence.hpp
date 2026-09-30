#pragma once
#include <cstdint>
namespace UnHoarder::WorldProbeEvidence {
// A code-reader invocation near a polled Win32 mouse-button edge is ONLY
// a temporal association. It is never proof of a D2R native pickup or target.
constexpr bool NearObservedClick(std::uint64_t sampledMs,
                                std::uint64_t clickMs) noexcept {
    return clickMs!=0 && sampledMs>=clickMs &&
        sampledMs-clickMs<=600;
}
}
