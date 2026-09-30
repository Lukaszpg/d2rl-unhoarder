#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

// Portable, read-only proof boundaries for correlating an append call with
// a subsequent render call. Pointer equality is NOT item-identity evidence.
namespace NativeRowAppendMatch {
inline constexpr std::size_t RowStride=0x2E8;
inline constexpr std::uint64_t MaxQueuedRows=4096;
inline constexpr std::uint64_t CapacityMask=0x7FFFFFFFFFFFFFFFULL;

constexpr bool ValidNewRow(std::uintptr_t data,
    std::uint64_t beforeCount,std::uint64_t afterCount,
    std::uint64_t rawCapacity) noexcept {
    return data && beforeCount<MaxQueuedRows && afterCount>0 &&
        afterCount==beforeCount+1 && afterCount<=MaxQueuedRows &&
        afterCount<=(rawCapacity&CapacityMask) &&
        afterCount-1<=(std::numeric_limits<std::uintptr_t>::max()-data)/
            RowStride;
}

constexpr bool RecentSameRow(std::uintptr_t appendRow,
    std::uintptr_t renderRow,std::uint32_t appendThread,
    std::uint32_t renderThread,std::int64_t appendQpc,
    std::int64_t renderQpc,std::int64_t frequency) noexcept {
    return appendRow && appendRow==renderRow && appendThread==renderThread &&
        appendQpc>0 && renderQpc>=appendQpc && frequency>=20 &&
        renderQpc-appendQpc<=frequency/20;
}
}
