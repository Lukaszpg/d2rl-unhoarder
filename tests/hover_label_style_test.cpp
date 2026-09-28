#include "src/hover_label_style.hpp"
#include <cassert>
#include <array>
#include <cstring>
#include <string_view>
using namespace SoE::LootFilter::HoverStyle;
int main() {
    constexpr std::string_view native = "\xEE\x81\xBE" "=Divine Orb";
    std::array<char, 256> out{};
    assert(Build(native, "DIVINE ORB - FILTERED", true, '1', out.data(),out.size()));
    const std::string_view expected="\xEE\x81\xBE" "=" "\xEE\x81\xBE" "1DIVINE ORB - FILTERED" "\xEE\x81\xBE" "0";
    assert(std::string_view(out.data())==expected);
    assert(Build(native,"EXO",true,0,out.data(),out.size()));
    assert(std::string_view(out.data())=="\xEE\x81\xBE" "=EXO");
    assert(!Build(native,"EXO",true,0,out.data(),4));
    assert(!Build("\xEE\x81\xBE" "=Divine\nOrb","EXO",true,0,out.data(),out.size()));
    assert(!Build(native,"",false,0,out.data(),out.size()));
    assert(PaletteSelector({1.f,0.f,0.f,1.f})=='1');
    assert(PaletteSelector({1.f,0.f,0.f,0.5f})=='\0');
    assert(PaletteSelector({0.5f,0.f,1.f,1.f})=='\0');
}
