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
    // Verify why code + normcode are ambiguous for a Normal item:
    // a base weapon repeats its own code in both fields, but its upgraded
    // variants retain the common normcode while their own code changes.
    constexpr auto Pack=[](char a,char b,char c) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(b))<<8U) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(c))<<16U) |
          0x20000000U;
    };
    std::array<std::array<unsigned char,32>,3> tierRows{};
    constexpr std::array<std::uint32_t,3> tierCodes{{
        Pack('h','a','x'),Pack('9','h','a'),Pack('7','h','a')
    }};
    for(std::size_t i=0;i<tierRows.size();++i) {
        std::memcpy(tierRows[i].data()+8,&tierCodes[i],4);
        const auto norm=Pack('h','a','x');
        std::memcpy(tierRows[i].data()+12,&norm,4);
    }
    // Both offsets match the Normal row; only +8 matches all three tiers.
    for(const std::size_t offset:{8U,12U})
        assert(FromCompiledRow(tierRows[0].data(),32,static_cast<std::uint32_t>(offset))==
               Canonical(tierCodes[0]));
    unsigned candidates=0;
    unsigned qualifiedOffset=0;
    for(std::uint32_t offset=0;offset<=28;++offset) {
        bool all=true;
        for(std::size_t i=0;i<tierRows.size();++i)
            if(FromCompiledRow(tierRows[i].data(),32,offset)!=Canonical(tierCodes[i])) {
                all=false; break;
            }
        if(all) {++candidates;qualifiedOffset=offset;}
    }
    assert(candidates==1 && qualifiedOffset==8);
    return 0;
}
