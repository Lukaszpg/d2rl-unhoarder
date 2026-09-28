#pragma once

// Build-93847 in-world-label relay text. This is deliberately a TEXT-ONLY
// transform: hidden-hover background/true RGBA must be qualified separately.
// Preserves the captured native four-byte prefix (U+E07E and its selector).
// No allocations, no source writes, no hard-coded item names.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace SoE::LootFilter::HoverStyle {

inline constexpr char Marker[] = "\xEE\x81\xBE";

// These named native color selectors are a limited palette, not a generic RGBA
// renderer. Do not quantize arbitrary RGBA colors without user permission.
inline char PaletteSelector(const std::array<float, 4>& rgba) noexcept {
    const auto near = [](float value, int byte) noexcept {
        return std::isfinite(value) && std::fabs(value - float(byte) / 255.0f) < 0.002f;
    };
    if (!near(rgba[3], 255)) return '\0';
    if (near(rgba[0], 255) && near(rgba[1], 0) && near(rgba[2], 0)) return '1'; // native red
    if (near(rgba[0], 0) && near(rgba[1], 0) && near(rgba[2], 255)) return '3'; // native blue
    if (near(rgba[0], 255) && near(rgba[1], 255) && near(rgba[2], 0)) return '9'; // native yellow
    if (near(rgba[0], 255) && near(rgba[1], 215) && near(rgba[2], 0)) return '4'; // native gold
    return '\0';
}

inline bool Build(std::string_view source, std::string_view name,
                  bool hasName, char palette, char* output,
                  std::size_t capacity) noexcept {
    if (!output || !capacity || source.empty() || source.size() > 255) return false;
    // The captured hover source starts with EE 81 BE 3D (not printable name
    // text). Preserve all four bytes verbatim instead of treating '=' as text.
    const bool nativePrefix = source.size() >= 5 &&
        std::memcmp(source.data(), Marker, 3) == 0;
    const std::size_t prefix = nativePrefix ? 4U : 0U;
    auto originalName = source.substr(prefix);
    if (originalName.empty() || originalName.find('\n') != std::string_view::npos ||
        originalName.find('\r') != std::string_view::npos ||
        originalName.find('\0') != std::string_view::npos ||
        originalName.find(std::string_view(Marker, 3)) != std::string_view::npos)
        return false;
    // Only one captured native formatting segment plus ordinary printable name.
    for (unsigned char ch : originalName)
        if (ch < 0x20 || ch == 0x7f) return false;
    if (hasName) {
        if (name.empty() || name.size() > 79 || name.find('\n') != name.npos ||
            name.find('\r') != name.npos || name.find('\0') != name.npos)
            return false;
    } else name = originalName;
    if (!hasName && !palette) return false;
    const std::size_t needed = prefix + (palette ? 4U : 0U) + name.size()
        + (palette ? 4U : 0U) + 1U;
    if (needed > capacity || needed > 256U) return false;
    std::size_t n{};
    if (prefix) { std::memcpy(output, source.data(), prefix); n += prefix; }
    if (palette) { std::memcpy(output + n, Marker, 3); n += 3; output[n++] = palette; }
    std::memcpy(output + n, name.data(), name.size()); n += name.size();
    if (palette) { std::memcpy(output + n, Marker, 3); n += 3; output[n++] = '0'; }
    output[n] = '\0';
    return source != std::string_view(output, n);
}
} // namespace SoE::LootFilter::HoverStyle
