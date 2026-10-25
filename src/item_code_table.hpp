#pragma once
// Copied item-code index from the active compiled Items table. No native
// pointer, SDK service or game memory is accessed by Lookup().
#include <cstdint>
#include <cstring>
#include <vector>
#include <cstddef>

namespace UnHoarder::ItemCodeTable {
struct Snapshot {
    std::vector<std::uint32_t> codes;
    std::uint64_t revision{};
    std::uint32_t rowSize{}, codeOffset{};
};
inline constexpr std::uint32_t Canonical(std::uint32_t code) noexcept {
    for (int i=3; i>=0; --i) {
        const auto byte=static_cast<unsigned char>(code>>(8U*i));
        if(byte!=0 && byte!=' ') break;
        code&=~(0xffU<<(8U*i));
    }
    return code;
}
inline constexpr bool Printable(std::uint32_t code) noexcept {
    code=Canonical(code);
    if(!code) return false;
    bool sawZero=false;
    for(unsigned i=0;i<4;++i) {
        const auto ch=static_cast<unsigned char>(code>>(8U*i));
        if(!ch) {sawZero=true;continue;}
        if(sawZero || ch<0x21 || ch>0x7e) return false;
    }
    return true;
}
inline constexpr std::uint32_t PadForSdk(std::uint32_t code) noexcept {
    code=Canonical(code);
    for(unsigned i=0;i<4;++i)
        if(!(code & (0xffU<<(8U*i)))) code|=0x20U<<(8U*i);
    return code;
}
inline std::uint32_t FromCompiledRow(const void* row,std::uint32_t stride,
                                    std::uint32_t offset) noexcept {
    if(!row || stride<4 || offset>stride-4)return 0;
    std::uint32_t raw{};
    std::memcpy(&raw,static_cast<const unsigned char*>(row)+offset,4);
    const auto code=Canonical(raw);
    return Printable(code)?code:0;
}
inline std::uint32_t Lookup(const Snapshot& table,std::uint32_t classId) noexcept {
    return classId<table.codes.size()?table.codes[classId]:0;
}
} // namespace UnHoarder::ItemCodeTable
