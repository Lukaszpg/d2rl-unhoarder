#pragma once
// Offline-safe bounded matching of a known native item ID against the bytes
// of ONE validated, synchronously borrowed native UI row. A coincident 32-bit
// match is a candidate field, never an item-ownership or write authorization.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NativeRowHandoffScan {
inline constexpr std::size_t RowStride = 0x2E8;
inline constexpr std::size_t MaxOffsets = 8;
struct Matches final {
    std::uint32_t total{};
    std::array<std::uint16_t,MaxOffsets> offsets{};
};
inline Matches FindAlignedId(const std::uint8_t* bytes,
    std::size_t length,std::uint32_t id) noexcept {
    Matches hits{};
    if (!bytes || id==0 || length>RowStride) return hits;
    for(std::size_t offset=0;offset+sizeof(id)<=length;offset+=alignof(std::uint32_t)) {
        std::uint32_t value{};
        std::memcpy(&value,bytes+offset,sizeof(value));
        if (value==id) {
            if (hits.total<hits.offsets.size())
                hits.offsets[hits.total]=static_cast<std::uint16_t>(offset);
            ++hits.total;
        }
    }
    return hits;
}
} // namespace NativeRowHandoffScan
