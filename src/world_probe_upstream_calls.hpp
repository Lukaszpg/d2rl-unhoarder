#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// Build-93847 *observed static CALL sites*, NOT verified function signatures.
// The source-to-producer-to-label ordering is drawn from the byte window
// captured by probe 0.2.18 (0x880960-0x880B60). Entry-byte and forwarding
// verification must succeed on the current game before using these addresses.
namespace WorldProbeUpstream {
struct Branch final {
    const char* name;
    std::uintptr_t sourceCall, expectedSource;
    std::uintptr_t producerCall, expectedProducer;
    std::uintptr_t forwardRva;
    std::array<std::uint8_t,3> expectedForward;
    std::uintptr_t formatterCall;
};
inline constexpr std::array<Branch,3> Branches{{
    {"first",0x880A91,0x8B2D0,0x880A9A,0xF1900,0x880AA2,{0x48,0x8B,0xF0},0x880AA5},
    {"second",0x880AC4,0x144640,0x880ACE,0x18D960,0x880AE0,{0x48,0x8B,0xC8},0x880AE3},
    {"third",0x880AE8,0x144640,0x880AF5,0x18DCA0,0x880B09,{0x48,0x8B,0xC8},0x880B0C},
}};
struct Target final { const char* name; std::uintptr_t rva; };
inline constexpr std::array<Target,5> Targets{{
    {"source-first",0x8B2D0},
    {"source-common",0x144640},
    {"producer-first",0xF1900},
    {"producer-second",0x18D960},
    {"producer-third",0x18DCA0},
}};
// No guess based only on E8: call site MUST also equal the expected
// target in the version-guarded native game image at runtime.
inline bool DecodeRelCall(std::span<const std::uint8_t> bytes,
                          std::uintptr_t callRva,
                          std::uintptr_t& targetRva) noexcept {
    targetRva=0;
    if(bytes.size()!=5 || bytes[0]!=0xE8 ||
       callRva>static_cast<std::uintptr_t>(
           std::numeric_limits<std::int64_t>::max()-5))return false;
    const auto raw=static_cast<std::uint32_t>(bytes[1]) |
        (static_cast<std::uint32_t>(bytes[2])<<8U) |
        (static_cast<std::uint32_t>(bytes[3])<<16U) |
        (static_cast<std::uint32_t>(bytes[4])<<24U);
    const auto offset=static_cast<std::int32_t>(raw);
    const auto signedRva=static_cast<std::int64_t>(callRva+5) +
        static_cast<std::int64_t>(offset);
    if(signedRva<0)return false;
    targetRva=static_cast<std::uintptr_t>(signedRva);
    return true;
}
} // namespace WorldProbeUpstream
