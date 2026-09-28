#include "src/native_row_append_match.hpp"
#include <cassert>
#include <cstdint>
#include <limits>
int main() {
    using namespace NativeRowAppendMatch;
    static_assert(RowStride==0x2E8);
    static_assert(ValidNewRow(0x10000,0,1,1));
    static_assert(ValidNewRow(0x10000,3,4,0x8000000000000004ULL));
    static_assert(!ValidNewRow(0,0,1,1));
    static_assert(!ValidNewRow(0x10000,3,3,4));
    static_assert(!ValidNewRow(0x10000,4096,4097,4097));
    static_assert(!ValidNewRow(std::numeric_limits<std::uintptr_t>::max()-4,0,2,2));
    static_assert(RecentSameRow(0x10000,0x10000,11,11,100000,100010,1000000));
    static_assert(!RecentSameRow(0x10000,0x10000,11,11,100000,200001,1000000));
    static_assert(!RecentSameRow(0x10000,0x10000,11,12,100000,100010,1000000));
    static_assert(!RecentSameRow(0x10000,0x10300,11,11,100000,100010,1000000));
    static_assert(!RecentSameRow(0x10000,0x10000,11,11,100010,100000,1000000));
}
