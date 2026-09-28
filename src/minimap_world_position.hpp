#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SoE::LootFilter::MinimapWorldPosition {

inline constexpr std::size_t MinimumPathBytes = 0x18;

struct Coordinates final {
    std::uint32_t x{};
    std::uint32_t y{};
    bool available{};
    bool plausible{};
};

inline Coordinates Decode(const std::uint8_t* path,std::size_t bytes) noexcept {
    Coordinates result{};
    if(!path || bytes<MinimumPathBytes) return result;
    std::memcpy(&result.x,path+0x10,4);
    std::memcpy(&result.y,path+0x14,4);
    result.available=true;
    result.plausible=result.x>0 && result.y>0 &&
        result.x<=0xFFFF && result.y<=0xFFFF;
    return result;
}

} // namespace SoE::LootFilter::MinimapWorldPosition
