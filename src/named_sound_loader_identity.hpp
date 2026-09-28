#pragma once
#include <cstdint>

// Known exact D2RLoader PE identities. PE recognition is not execution
// authority: the caller MUST also verify the live native sound call chain.
namespace SoundLoaderIdentity {
enum class Layout : std::uint8_t {Unknown, Loader130, Loader131};
[[nodiscard]] constexpr Layout Classify(std::uint32_t timestamp,
                                        std::uint32_t imageSize) noexcept {
    if(timestamp==0x6AAFC972U && imageSize==0x5602000U)
        return Layout::Loader130;
    if(timestamp==0x6AB3782CU && imageSize==0x5643000U)
        return Layout::Loader131;
    return Layout::Unknown;
}
[[nodiscard]] constexpr const char* Name(Layout layout) noexcept {
    switch(layout) {
      case Layout::Loader130: return "loader-1.3.0";
      case Layout::Loader131: return "loader-1.3.1";
      default: return "unknown";
    }
}
}
