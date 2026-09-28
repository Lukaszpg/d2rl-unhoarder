#pragma once
// The SoE V1 label event carries the original text; SoE V2 may replace it
// before the styled-text append. Never treat a same-name V2 callback for a
// different unit as proof of ownership of the native row.
#include <cstdint>
#include <string_view>
namespace NativeRowLiveDisplayMatch {
inline bool SameEvent(std::uint32_t observedClass,
    std::uint32_t observedId,std::uint32_t observedCode,
    std::string_view original,std::uint32_t v2Class,
    std::uint32_t v2Id,std::uint32_t v2Code,
    std::string_view v2Original) noexcept {
    return observedId!=0 && observedId==v2Id &&
        observedClass==v2Class && observedCode!=0 &&
        observedCode==v2Code && !original.empty() &&
        original==v2Original;
}
} // namespace NativeRowLiveDisplayMatch
