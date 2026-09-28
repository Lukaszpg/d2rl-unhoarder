#include "src/ground_quantity_label.hpp"
#include <array>
#include <cassert>
#include <string_view>
using namespace SoE::LootFilter::GroundQuantity;
int main(){
    std::array<char,256> out{}; std::size_t n{};
    assert(!Append("Divine Orb",0,out.data(),out.size(),n));
    assert(!Append("Divine Orb",1,out.data(),out.size(),n));
    assert(Append("Divine Orb",3,out.data(),out.size(),n));
    assert(std::string_view(out.data())=="3x Divine Orb");
    assert(Append("Exalted Orb",12,out.data(),out.size(),n));
    assert(std::string_view(out.data())=="12x Exalted Orb");
    assert(!Append("3x Divine Orb",3,out.data(),out.size(),n));
    assert(Append("Divine Orb (3)",3,out.data(),out.size(),n));
    assert(std::string_view(out.data())=="3x Divine Orb");
    assert(!Append("Divine Orb (12)",3,out.data(),out.size(),n));
    assert(!Append("Divine Orb",65536,out.data(),out.size(),n));
    assert(!Append("Divine Orb",33,out.data(),8,n));
    assert(BuildHover("\xEE\x81\xBE" "=Divine Orb",{},false,0,3,out.data(),out.size()));
    assert(std::string_view(out.data())=="\xEE\x81\xBE" "=3x Divine Orb");
    assert(BuildHover("\xEE\x81\xBE" "=Divine Orb", "CUSTOM",true,'1',12,out.data(),out.size()));
    assert(std::string_view(out.data())=="\xEE\x81\xBE" "=" "\xEE\x81\xBE" "1" "12x CUSTOM" "\xEE\x81\xBE" "0");
    assert(!BuildHover("\xEE\x81\xBE" "=Divine Orb",{},false,0,1,out.data(),out.size()));
    assert(!BuildHover("\xEE\x81\xBE" "=3x Divine Orb",{},false,0,3,out.data(),out.size()));
}
