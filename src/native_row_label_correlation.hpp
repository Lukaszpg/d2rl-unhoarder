#pragma once
#include <cstdint>
#include <string_view>

// Diagnostic-only comparison of a SoE V1 item-label event with native
// render-row text captured during a bounded phase. This never proves the
// native row is owned by the event's UnitAny: names and UI slots can repeat.
namespace NativeRowLabelCorrelation {
enum class Relation : std::uint8_t {
    Unavailable=0, Exact=1, ColorPrefixOnly=2, Different=3
};
inline std::string_view StripObservedColorPrefix(std::string_view s) noexcept {
    // Build-93847 SoE/D2R examples start with EE 81 BE <native-color>.
    // Do not strip arbitrary UTF-8 or guess other native control sequences.
    if (s.size()>=4 &&
        static_cast<unsigned char>(s[0])==0xEE &&
        static_cast<unsigned char>(s[1])==0x81 &&
        static_cast<unsigned char>(s[2])==0xBE)
        s.remove_prefix(4);
    return s;
}
inline Relation Compare(std::string_view nativeText,
                        std::string_view labelEventText) noexcept {
    if (nativeText.empty() || labelEventText.empty()) return Relation::Unavailable;
    if (nativeText==labelEventText) return Relation::Exact;
    const auto a=StripObservedColorPrefix(nativeText);
    const auto b=StripObservedColorPrefix(labelEventText);
    if (a==b && !a.empty()) return Relation::ColorPrefixOnly;
    return Relation::Different;
}
inline const char* Name(Relation r) noexcept {
    switch(r) {
        case Relation::Exact: return "exact-bytes";
        case Relation::ColorPrefixOnly: return "same-text-different-native-color";
        case Relation::Different: return "different-text";
        default: return "unavailable";
    }
}
} // namespace NativeRowLabelCorrelation
