#include <windows.h>
#include "ground_action_demand.hpp"
#include "ground_property_reader.hpp"
#include "ground_property_live_policy.hpp"
#include "ground_ethereal_policy.hpp"
#include "ground_identified_policy.hpp"
#include "guarded_local_read.hpp"
#include "loot_performance.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
namespace RuleEngine=UnHoarder::RuleEngine;
namespace GroundPropertyReader=UnHoarder::GroundPropertyReader;
namespace GroundPropertyLive=UnHoarder::GroundPropertyLive;
namespace GroundEthereal=UnHoarder::GroundEthereal;
namespace GroundIdentified=UnHoarder::GroundIdentified;
namespace Perf=UnHoarder::Perf;
struct Rule {
    bool usesConditions{true},continueEvaluation{},show{true},hasName{},hasMinimapIcon{};
    std::uint32_t code{},name{},icon{};RuleEngine::Conditions conditions{};
};
struct FilterRuleTable {
    std::vector<Rule> rules;
    bool usesQuantity{},usesQuality{true},usesItemLevel{true},usesSockets{true},usesEthereal{true};
    std::uint64_t generation{1};
};
int contextValue{};const int* Context=&contextValue;
const char* ResolveBuildName(const int*) {return "93847";}
bool IsSupportedBuild(const char* name) {return std::string_view(name)=="93847";}
std::uint32_t CanonicalItemCode(std::uint32_t code) {return code;}
std::atomic<void*> GroundQuantityReader{};
std::uint32_t GroundStackQuantity(const void*) {return 1;}
bool qualityAvailable=true,socketsAvailable=true;
unsigned socketReads{},memoryReads{},apiReads{},localReads{},pageChecks{},mutateAfter{};
unsigned protectAfterRead{},protectAfterPage{};
unsigned char* mutateAt{};unsigned char* blocked{};
std::uint32_t nativeSockets{};
static BOOL WINAPI CountRead(HANDLE process,LPCVOID source,LPVOID output,SIZE_T size,SIZE_T* copied) {
    ++memoryReads;++apiReads;
    if(!qualityAvailable && source==blocked)return FALSE;
    const auto result=ReadProcessMemory(process,source,output,size,copied);
    if(memoryReads==mutateAfter && mutateAt)*mutateAt^=1;
    if(memoryReads==protectAfterRead && blocked){DWORD old{};VirtualProtect(blocked,0x1000,PAGE_NOACCESS,&old);}
    return result;
}
static SIZE_T WINAPI CountPage(LPCVOID source,PMEMORY_BASIC_INFORMATION output,SIZE_T size) {
    ++pageChecks;const auto result=VirtualQuery(source,output,size);
    if(pageChecks==protectAfterPage && blocked){DWORD old{};VirtualProtect(blocked,0x1000,PAGE_NOACCESS,&old);}
    return result;
}
namespace FixtureLocalRead::Detail {
bool CopyReadable(const void* source,void* output,SIZE_T size) noexcept {
    ++memoryReads;++localReads;
    if(!qualityAvailable && source==blocked)return false;
    const auto result=GuardedLocalRead::Detail::CopyReadable(source,output,size);
    if(memoryReads==mutateAfter && mutateAt)*mutateAt^=1;
    return result;
}
}
#define ReadProcessMemory CountRead
#define VirtualQuery CountPage
namespace FixturePageQuery {
bool SinglePage(std::uintptr_t address,SIZE_T bytes,std::uintptr_t owner,DWORD& protection) noexcept {
    const bool result=CurrentPageQuery::SinglePage(address,bytes,owner,protection);
    if(result){
        ++pageChecks;
        if(pageChecks==protectAfterPage && blocked){DWORD old{};VirtualProtect(blocked,0x1000,PAGE_NOACCESS,&old);}
    }
    return result;
}
}
#define CurrentPageQuery FixturePageQuery
#include "ground_page_reader.inc"
#undef CurrentPageQuery
#undef VirtualQuery
#define GuardedLocalRead FixtureLocalRead
#include "ground_quality_reader.inc"
#undef GuardedLocalRead
bool ReadNativeGroundSockets(const void*,std::uint32_t,std::uint32_t,GroundPropertyLive::Purpose,std::uint32_t& sockets) noexcept {
    ++socketReads;sockets=nativeSockets;return socketsAvailable;
}
bool ReadNativeVendorSellPrice(const void*,std::uint32_t,std::uint32_t,std::uint32_t,
    GroundPropertyLive::Purpose,std::uint32_t&,RuleEngine::Item*,std::uint64_t) noexcept {return false;}
#include "ground_rule_item.inc"
#undef ReadProcessMemory
static void Check(bool ok,const char* what) {if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}}
struct Geometry {std::uint32_t name{},icon{};bool operator==(const Geometry&) const=default;};
Geometry Resolve(const FilterRuleTable& table,const RuleEngine::Item& item) {
    Geometry result{};bool show=true;
    RuleEngine::ResolveMatchingRulesFailOpen(table.rules,item,[&](const Rule& rule) {
        show=rule.show;if(rule.hasName)result.name=rule.name;if(rule.hasMinimapIcon)result.icon=rule.icon;
    },[&] {show=true;});
    if(!show)result.icon=0;return result;
}
int main() {
    auto* storage=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x2000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    Check(storage!=nullptr,"owned item source");
    auto* header=reinterpret_cast<std::uint32_t*>(storage);auto* data=storage+0x1000;blocked=data;
    header[0]=4;header[1]=42;header[2]=17;header[3]=3;std::memcpy(storage+0x10,&data,sizeof(data));
    auto* fields=reinterpret_cast<std::uint32_t*>(data);fields[0]=2;fields[0x38/4]=99;
    memoryReads=apiReads=localReads=pageChecks=0;
    const auto read=ReadNativeGroundQualityLevel(storage,17,42,GroundPropertyLive::Purpose::VerifiedLabel);
    Check(read.qualityKnown && memoryReads==4,"both unit and property snapshots retained");
    Check(apiReads==2 && localReads==2 && pageChecks==2,
        "two fresh page-qualified property copies replace redundant API reads");
    Rule unique{};unique.hasMinimapIcon=true;unique.icon=9;unique.conditions.qualities={7};
    Rule sockets{};sockets.conditions.sockets.enabled=true;sockets.conditions.sockets.hasMin=true;sockets.conditions.sockets.min=1;
    Rule hide{};hide.show=false;hide.conditions.etherealEnabled=true;hide.conditions.etherealExpected=false;
    FilterRuleTable table{{unique,sockets,hide}};
    socketReads=0;
    const auto ordinary=GroundRuleItem(123,storage,&table,17,GroundPropertyLive::Purpose::VerifiedLabel,true);
    Check(ordinary.qualityKnown && socketReads==0,"no geometry action can match ordinary item; unrelated socket guard is not invoked");
    socketReads=0;(void)GroundRuleItem(123,storage,&table,17,GroundPropertyLive::Purpose::VerifiedLabel);
    Check(socketReads==1,"visibility and identity retain the original complete native-property path");
    unsigned cases{};
    for(unsigned continueBits=0;continueBits<8;++continueBits)
    for(unsigned quality=1;quality<=9;++quality)
    for(unsigned socketsValue=0;socketsValue<3;++socketsValue)
    for(unsigned ethereal=0;ethereal<2;++ethereal)
    for(unsigned known=0;known<4;++known) {
        table.rules[0].continueEvaluation=(continueBits&1)!=0;
        table.rules[1].continueEvaluation=(continueBits&2)!=0;
        table.rules[2].continueEvaluation=(continueBits&4)!=0;
        qualityAvailable=(known&1)!=0;socketsAvailable=(known&2)!=0;
        fields[0]=quality;fields[0x18/4]=ethereal?0x00400000U:0;nativeSockets=socketsValue;
        for(unsigned nameRule=0;nameRule<2;++nameRule) {
            table.rules[1].hasName=nameRule!=0;table.rules[1].name=55;
            const auto full=GroundRuleItem(123,storage,&table,17,GroundPropertyLive::Purpose::VerifiedLabel);
            const auto pruned=GroundRuleItem(123,storage,&table,17,GroundPropertyLive::Purpose::VerifiedLabel,true);
            Check(Resolve(table,full)==Resolve(table,pruned),"geometry output parity across Continue, unknowns, visibility and properties");++cases;
        }
    }
    qualityAvailable=true;socketsAvailable=true;mutateAt=nullptr;fields[0]=7;header[2]=17;
    const auto valid=ReadNativeGroundQualityLevel(storage,17,42,GroundPropertyLive::Purpose::VerifiedLabel);
    Check(valid.qualityKnown,"warm native quality snapshot");
    memoryReads=0;mutateAfter=2;mutateAt=storage+8;
    Check(!ReadNativeGroundQualityLevel(storage,17,42,GroundPropertyLive::Purpose::VerifiedLabel).qualityKnown,
        "native identity changes between snapshots still refused");
    header[2]=17;memoryReads=0;mutateAt=data;
    Check(!ReadNativeGroundQualityLevel(storage,17,42,GroundPropertyLive::Purpose::VerifiedLabel).qualityKnown,
        "quality changes between snapshots still refused");
    mutateAt=nullptr;mutateAfter=0;fields[0]=7;DWORD old{};
    for(unsigned flags:{0x10U,0x00400000U}) {
        fields[0x18/4]=flags;memoryReads=0;mutateAfter=2;mutateAt=data+0x18;
        const auto result=ReadNativeGroundQualityLevel(storage,17,42,GroundPropertyLive::Purpose::VerifiedLabel,nullptr,true,true);
        Check(!result.etherealKnown && !result.identifiedKnown,"requested flags changing between observations are refused");
    }
    mutateAt=nullptr;mutateAfter=0;
    for(unsigned fault:{1U,2U,3U}) {
        memoryReads=pageChecks=0;protectAfterRead=fault==3?3:0;protectAfterPage=fault<3?fault:0;
        const char* reason{};
        Check(!ReadNativeGroundQualityLevel(storage,17,42,GroundPropertyLive::Purpose::VerifiedLabel,&reason).qualityKnown,
            "protection changes after either qualification or between observations refuse safely");
        Check(std::string_view(reason)==(fault==1?"item-data-first-read-failed":fault==2?"item-data-second-read-failed":"item-data-second-page-not-readable"),
            "fault is caught at the intended observation boundary");
        Check(VirtualProtect(data,0x1000,PAGE_READWRITE,&old)!=0,"restore fixture after protected-copy race");
    }
    protectAfterRead=protectAfterPage=0;
    Check(VirtualProtect(data,0x1000,PAGE_NOACCESS,&old)!=0,"invalidate item data after warmup");
    Check(!ReadNativeGroundQualityLevel(storage,17,42,GroundPropertyLive::Purpose::VerifiedLabel).qualityKnown,
        "no cached property or page proof after warmup");
    VirtualFree(storage,0,MEM_RELEASE);
    std::printf("%u production property/geometry parity cases and live mutation refusals passed.\n",cases);
    return 0;
}
