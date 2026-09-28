#pragma once

#include <cstdint>

namespace SoE::LootFilter::GroundPropertyReader {

constexpr std::uint32_t ReadLe32(const std::uint8_t* bytes) noexcept {
    return std::uint32_t(bytes[0]) |
        (std::uint32_t(bytes[1])<<8U) |
        (std::uint32_t(bytes[2])<<16U) |
        (std::uint32_t(bytes[3])<<24U);
}

} // namespace SoE::LootFilter::GroundPropertyReader
