#include "src/ground_quantity_label.hpp"
#include "src/native_row_live_display_match.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <string_view>
int main() {
 using SoE::LootFilter::GroundQuantity::BuildHover;
 std::array<char,256> transform{};
 const auto v1=std::string_view("\xEE\x81\xBE" "=Divine Orb");
 assert(BuildHover(v1,{},false,0,4,transform.data(),transform.size()));
 const auto v2=std::string_view(transform.data());
 assert(v2=="\xEE\x81\xBE" "=4x Divine Orb");
 assert(v1!=v2); // 0.2.5/0.2.6 rejected the changed row here.
 assert(NativeRowLiveDisplayMatch::SameEvent(345,77,0x6f766964,v1,
     345,77,0x6f766964,v1));
 assert(!NativeRowLiveDisplayMatch::SameEvent(345,77,0x6f766964,v1,
     345,78,0x6f766964,v1));
 std::array<char,256> colored{};
 assert(BuildHover(v1,"CUSTOM ORB",true,'1',12,colored.data(),colored.size()));
 const auto actual=std::string_view(colored.data());
 assert(actual=="\xEE\x81\xBE" "=" "\xEE\x81\xBE" "1" "12x CUSTOM ORB" "\xEE\x81\xBE" "0");
 assert(actual.size()<=64); // current native text witness capacity
 assert(!BuildHover(v1,{},false,0,1,transform.data(),transform.size()));
}
