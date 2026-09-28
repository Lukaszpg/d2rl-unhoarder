#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

// Header-only so the portable contract tests and the Windows plugin use the
// EXACT same decoder. These are 93847 observations, not a general ABI claim.
namespace NativeRowStringLayout {
constexpr std::uint64_t InlineTag=UINT64_C(1)<<63;
constexpr std::uint64_t MaxTextBytes=1U<<20;
constexpr std::uint64_t InlineCapacity=15;
struct Decoded final {
    std::uintptr_t pointer{};
    std::uint64_t size{},capacity{},encodedCapacity{};
    bool isInline{},valid{};
};
inline Decoded Decode(const std::array<std::uint8_t,0x50>& bytes,
                      std::size_t offset,
                      std::uintptr_t elementAddress) noexcept {
    Decoded out{};
    if (offset>bytes.size()-0x28 ||
        elementAddress>std::numeric_limits<std::uintptr_t>::max()-offset-0x18)
        return out;
    const auto* p=bytes.data()+offset;
    std::memcpy(&out.pointer,p,sizeof(out.pointer));
    std::memcpy(&out.size,p+0x08,sizeof(out.size));
    std::memcpy(&out.encodedCapacity,p+0x10,sizeof(out.encodedCapacity));
    out.isInline=(out.encodedCapacity&InlineTag)!=0;
    out.capacity=out.encodedCapacity&~InlineTag;
    if (out.size>MaxTextBytes || out.size>out.capacity ||
        out.capacity>MaxTextBytes) return out;
    if (out.isInline) {
        out.valid=out.capacity==InlineCapacity &&
            out.pointer==elementAddress+offset+0x18;
    } else {
        out.valid=out.capacity>=16 && out.pointer!=0;
    }
    return out;
}
} // namespace NativeRowStringLayout
