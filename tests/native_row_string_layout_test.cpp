#include "src/native_row_string_layout.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
int main() {
    constexpr std::uintptr_t row=0xC4D20FE8;
    std::array<std::uint8_t,0x50> raw{};
    const auto put=[&](std::size_t at,std::uint64_t n) {
        std::memcpy(raw.data()+at,&n,8);
    };
    // Live hidden-hover shape: +0x00 is a tagged short string,
    // pointer points to its own 16-byte inline buffer at +0x18.
    put(0x00,row+0x18);put(0x08,14);
    put(0x10,NativeRowStringLayout::InlineTag|15);
    constexpr char label[]="\xEE\x81\xBE=Divine Orb";
    static_assert(sizeof(label)-1==14);
    std::memcpy(raw.data()+0x18,label,14);
    auto a=NativeRowStringLayout::Decode(raw,0,row);
    assert(a.valid && a.isInline && a.size==14 && a.capacity==15);
    assert(std::memcmp(raw.data()+0x18,label,14)==0);
    // Second string: empty inline field at +0x28, internal storage +0x40.
    put(0x28,row+0x40);put(0x30,0);
    put(0x38,NativeRowStringLayout::InlineTag|15);
    a=NativeRowStringLayout::Decode(raw,0x28,row);
    assert(a.valid && a.isInline && a.size==0);
    // Heap shape seen in inventory phase (length == capacity allowed).
    put(0x00,0x2A8E967E0);put(0x08,36);put(0x10,36);
    a=NativeRowStringLayout::Decode(raw,0,row);
    assert(a.valid && !a.isInline && a.size==36 && a.capacity==36);
    // Fail closed on wrong inline pointer / invalid length / huge capacity.
    put(0x00,row+0x20);put(0x08,14);put(0x10,NativeRowStringLayout::InlineTag|15);
    assert(!NativeRowStringLayout::Decode(raw,0,row).valid);
    put(0x00,row+0x18);put(0x08,16);
    assert(!NativeRowStringLayout::Decode(raw,0,row).valid);
    put(0x00,0x2A8E967E0);put(0x08,36);put(0x10,1ULL<<21);
    assert(!NativeRowStringLayout::Decode(raw,0,row).valid);
    std::cout<<"0.1.95 tagged native string layout test: PASS\n";
}
