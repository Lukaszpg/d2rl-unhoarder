#include "src/native_row_bg_policy.hpp"
#include <cassert>
int main() {
    using namespace NativeRowBgPolicy;
    static_assert(SingleRow(0,1));
    static_assert(!SingleRow(1,2));
    static_assert(!SingleRow(0,2));
    static_assert(VanillaHiddenBlack({{0,0,0,0x3F19999A}}));
    static_assert(!VanillaHiddenBlack({{0,0,0,0x3F400000}})); // inventory .75
    static_assert(!VanillaHiddenBlack({{1,0,0,0x3F19999A}}));
    static_assert(RecentQpc(100,120,10000));
    static_assert(!RecentQpc(100,121,10000));
    static_assert(!RecentQpc(100,99,10000));
    static_assert(!RecentQpc(0,0,10000));
    static_assert(!RecentQpc(100,100,0));
    return 0;
}
