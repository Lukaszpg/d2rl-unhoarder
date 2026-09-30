#include "src/native_row_live_display_match.hpp"
#include <cassert>
using NativeRowLiveDisplayMatch::SameEvent;
int main(){
 const auto original=std::string_view("\xEE\x81\xBE" "=Divine Orb");
 assert(SameEvent(9,112,0x6f766964,original,9,112,0x6f766964,original));
 assert(!SameEvent(9,112,0x6f766964,original,9,113,0x6f766964,original));
 assert(!SameEvent(9,112,0x6f766964,original,10,112,0x6f766964,original));
 assert(!SameEvent(9,112,0x6f766964,original,9,112,0x6f766965,original));
 assert(!SameEvent(9,112,0x6f766964,original,9,112,0x6f766964,"Divine Orb"));
}
