#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace SoE::LootFilter::MinimapWorldPositionProbe {
// SoE's portal witness reads D2StaticPathStrc::tGameCoord at +0x10.
// Applying that layout to a ground item is a hypothesis pending game evidence.
inline constexpr std::size_t MinimumPathBytes=0x18;
struct Candidate {
    std::uint32_t x{},y{};
    bool available{},plausible{};
};
inline Candidate Decode(const std::uint8_t* path,std::size_t bytes) noexcept {
    Candidate result{};
    if(!path || bytes<MinimumPathBytes) return result;
    std::memcpy(&result.x,path+0x10,4);
    std::memcpy(&result.y,path+0x14,4);
    result.available=true;
    result.plausible=result.x>0 && result.y>0 &&
        result.x<=0xFFFF && result.y<=0xFFFF;
    return result;
}
// Keep separate samples for mode 5/3 and for a redrop at a new coordinate.
struct Key { std::uint32_t code{},id{},classId{},mode{},x{},y{},status{}; };
inline bool SameSample(const Key& a,const Key& b) noexcept {
    return a.code==b.code && a.id==b.id && a.classId==b.classId &&
        a.mode==b.mode && a.x==b.x && a.y==b.y && a.status==b.status;
}
} // namespace SoE::LootFilter::MinimapWorldPositionProbe
