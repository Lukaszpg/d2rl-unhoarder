#include "item_code_table.hpp"
#include <cassert>
#include <array>
using namespace UnHoarder::ItemCodeTable;
int main(){
    constexpr std::uint32_t capRaw=0x20706163U;
    static_assert(Canonical(capRaw)==0x00706163U);
    static_assert(PadForSdk(Canonical(capRaw))==capRaw);
    static_assert(Printable(capRaw));
    static_assert(!Printable(0));
    static_assert(!Printable(0x001d6163U));
    static_assert(!Printable(0x61006263U));
    std::array<unsigned char,32> row{};
    row[8]='c';row[9]='a';row[10]='p';row[11]=' ';
    assert(FromCompiledRow(row.data(),row.size(),8)==Canonical(capRaw));
    assert(FromCompiledRow(row.data(),row.size(),29)==0);
    assert(FromCompiledRow(nullptr,row.size(),8)==0);
    assert(FromCompiledRow(row.data(),3,0)==0);
    assert(FromCompiledRow(row.data(),row.size(),31)==0);
    Snapshot snapshot{};
    snapshot.revision=17;snapshot.codes={0x00706163U,0x006e6972U};
    assert(Lookup(snapshot,0)==snapshot.codes[0]);
    assert(Lookup(snapshot,1)==snapshot.codes[1]);
    assert(Lookup(snapshot,2)==0);
    assert(Lookup(snapshot,0xffffffffU)==0);
    return 0;
}
