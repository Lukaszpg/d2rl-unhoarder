#include "src/native_row_bg_live_policy.hpp"
#include <cassert>
using namespace NativeRowBgLivePolicy;
static_assert(RecentChain(100,110,5000));
static_assert(RecentChain(100,110,5000));
static_assert(!RecentChain(100,111,5000));
static_assert(!RecentChain(101,100,5000));
static_assert(!RecentChain(100,101,0));
static_assert(SameLabel(7,7,120,120,104,104,706,706,0x6f766964,0x6f766964));
static_assert(!SameLabel(7,8,120,120,104,104,706,706,0x6f766964,0x6f766964));
static_assert(!SameLabel(7,7,120,121,104,104,706,706,0x6f766964,0x6f766964));
static_assert(!SameLabel(7,7,120,120,104,103,706,706,0x6f766964,0x6f766964));
static_assert(!SameLabel(7,7,120,120,104,104,706,706,0x6f766964,0x6f766965));
int main(){ assert(RecentChain(1,1,5000)); }
