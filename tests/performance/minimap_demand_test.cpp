#include "ground_quantity_label.hpp"
#include "hover_label_style.hpp"
#include "../../interop/in_world_item_label_compat_v1.hpp"
#include <windows.h>
#include "filter_rule_engine.hpp"
#include "ground_property_live_policy.hpp"
#include "minimap_world_position.hpp"
#include "minimap_overlay_renderer.hpp"
#include "loot_performance.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
namespace RuleEngine=UnHoarder::RuleEngine;
namespace GroundPropertyLive=UnHoarder::GroundPropertyLive;
namespace GroundQuantity=UnHoarder::GroundQuantity;
namespace MinimapWorldPosition=UnHoarder::MinimapWorldPosition;
namespace MinimapOverlayRenderer=UnHoarder::MinimapOverlayRenderer;
namespace MinimapIconPolicy=UnHoarder::MinimapIconPolicy;
namespace Perf=UnHoarder::Perf;
namespace HoverStyle=UnHoarder::HoverStyle;
namespace InWorldCompat=D2RLInterop::InWorldItemLabelCompatV1;
constexpr size_t CandidateTextMaximum=96,InnerNameBufferBytes=0x80,InnerNamePrefixBytes=4;
constexpr size_t MinimapProjectionItemCapacity=256,MinimapUnitBytes=0x40,MinimapPathBytes=0x28;
constexpr uintptr_t Base=0x140000000,InnerNameWriterCallerRva=0x1FAA18,DirectCallBytes=5;
#include "label_rule_types.inc"
std::atomic<std::shared_ptr<const FilterRuleTable>> PublishedFilterRules;
enum class GeometryMode {Off,Rules};
std::atomic<GeometryMode> ActiveGeometryMode{GeometryMode::Rules};
std::atomic_bool ItemCodeTableReadyMock{true},AutomapProjectionArmed{true};
struct OwnedScope {bool owned{};bool Owns(const void*,const void*,uint32_t)const{return owned;}} ControllerGroundScope;
constexpr uint32_t Gem=0x00726d67;
uint32_t CanonicalItemCode(uint32_t value){return value;}
bool PrintableItemCode(uint32_t value){return value==Gem;}
uint32_t ReadCode(void*){return Gem;}
std::uint32_t LookupItemCode(std::uint32_t) noexcept {return ReadCode(nullptr);}
bool ItemCodeTableReady() noexcept {return ItemCodeTableReadyMock.load();}
uint32_t fixtureQuantity=0,quality=2;
bool qualityKnown=true;
uint32_t GroundStackQuantity(const void*){return fixtureQuantity;}
RuleEngine::Item GroundRuleItem(uint32_t code,const void*,const FilterRuleTable*,uint32_t,GroundPropertyLive::Purpose,bool=false) {
    RuleEngine::Item item{};item.code=code;item.qualityKnown=qualityKnown;item.quality=quality;return item;
}
unsigned checks{},nativeCalls{},positionReads{},mutation{};
unsigned observedLabels{},observedReplacements{};
void ObserveNativeRowLiveLabel(int32_t,uint32_t,uint32_t,uint32_t,const void*,const char*,uint32_t){++observedLabels;}
void ObserveNativeRowLiveReplacement(uint32_t,uint32_t,uint32_t,std::string_view,std::string_view){++observedReplacements;}
alignas(16) std::array<uint8_t,0x40> unitBytes{};
alignas(16) std::array<uint8_t,0x28> pathBytes{};
alignas(16) std::array<uint8_t,0x100> recordBytes{};
uint64_t NativeName(void*,void* output,uint32_t,void*) {
    ++nativeCalls;std::memcpy(static_cast<char*>(output)+4,"Gem",4);return 123;
}
auto OriginalInnerNameWriter=&NativeName;
void* TestReturnAddress(){return reinterpret_cast<void*>(Base+InnerNameWriterCallerRva+5);}
static BOOL WINAPI CountRead(HANDLE process,LPCVOID source,LPVOID out,SIZE_T size,SIZE_T* copied) {
    const bool position=(source==unitBytes.data() && size==MinimapUnitBytes) ||
        (source==pathBytes.data() && size==MinimapPathBytes);
    if(position)++positionReads;
    const auto ok=ReadProcessMemory(process,source,out,size,copied);
    if(positionReads==2 && position) {
        if(mutation==1)unitBytes[8]^=1;
        if(mutation==2)pathBytes[0x10]^=1;
    }
    return ok;
}
#define ReadProcessMemory CountRead
#include "ground_page_reader.inc"
#include "minimap_state.inc"
#include "minimap_updates.inc"
#include "minimap_observation.inc"
#include "label_rule_resolution.inc"
#define _ReturnAddress TestReturnAddress
#include "inner_name_runtime.inc"
#include "compatible_label_runtime.inc"
#undef _ReturnAddress
#undef ReadProcessMemory
static void Check(bool ok,const char* what){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s (position reads=%u)\n",what,positionReads);std::exit(1);}}
void Reset() {
    unitBytes={};pathBytes={};recordBytes={};mutation=positionReads=nativeCalls=0;
    auto header=reinterpret_cast<uint32_t*>(unitBytes.data());header[0]=4;header[1]=42;header[2]=17;header[3]=3;
    const auto path=pathBytes.data();std::memcpy(unitBytes.data()+0x38,&path,sizeof(path));
    const uint32_t x=100,y=200;std::memcpy(pathBytes.data()+0x10,&x,4);std::memcpy(pathBytes.data()+0x14,&y,4);
    std::memcpy(recordBytes.data()+0x10,&header[2],4);
    MinimapProjectionItems={};MinimapProjectionItemCount=0;fixtureQuantity=0;quality=2;qualityKnown=true;
    ActiveGeometryMode=GeometryMode::Rules;ItemCodeTableReadyMock=true;AutomapProjectionArmed=true;ControllerGroundScope.owned=false;
}
void Label() {
    Check(HookInnerNameWriter(unitBytes.data(),recordBytes.data()+0x24,0x80,nullptr)==123,"native writer return unchanged");
}
int main() {
    auto table=std::make_shared<FilterRuleTable>();FilterNameRule unique{};unique.usesConditions=true;unique.conditions.qualities={7};unique.hasMinimapIcon=true;
    FilterNameRule gem{};gem.usesConditions=true;gem.conditions.codes={Gem};table->rules={unique,gem};PublishedFilterRules=table;
    Reset();for(unsigned i=0;i<1800;++i)Label();
    Check(positionReads==0,"ordinary gem labels avoid all four unused minimap position reads");
    Check(nativeCalls==1800 && !std::strcmp(reinterpret_cast<char*>(recordBytes.data()+0x28),"Gem"),"native naming preserved");
    Reset();quality=7;Label();
    Check(positionReads==4 && MinimapProjectionItemCount==1 && MinimapProjectionItems[0].hasIcon &&
        MinimapProjectionItems[0].worldX==100 && MinimapProjectionItems[0].worldY==200,"icon-bearing label retains complete double position observation");
    Reset();quality=7;ControllerGroundScope.owned=true;const uint32_t unpublished=0xFFFFFFFFu;std::memcpy(recordBytes.data()+0x10,&unpublished,4);Label();
    Check(positionReads==4 && MinimapProjectionItemCount==1 && MinimapProjectionItems[0].hasIcon && MinimapProjectionItems[0].observedTick &&
        MinimapProjectionItems[0].worldX==100 && MinimapProjectionItems[0].worldY==200,"controller-scoped label observes minimap position before recordId is published");
    quality=2;positionReads=0;Label();Check(positionReads==0 && !MinimapProjectionItems[0].hasIcon,"no-icon decision clears former icon without sampling position");
    qualityKnown=false;positionReads=0;Label();Check(positionReads==0 && !MinimapProjectionItems[0].hasIcon,"unresolved rule retains native label and no staged icon");
    Reset();quality=7;unitBytes[12]=5;Label();Check(positionReads==4 && MinimapProjectionItems[0].observedTick,"verified presentation mode still observed");
    for(unsigned change:{1U,2U}){Reset();quality=7;mutation=change;Label();Check(positionReads==4 && !MinimapProjectionItems[0].observedTick,"identity/path mutation still refuses coordinates");}
    Reset();fixtureQuantity=5;Label();Check(!std::strcmp(reinterpret_cast<char*>(recordBytes.data()+0x28),"5x Gem"),"quantity prefix preserved without icon");
    Reset();ActiveGeometryMode=GeometryMode::Off;Label();Check(positionReads==0 && nativeCalls==1,"inactive path forwards original once");
    Reset();InWorldCompat::Event event{};event.structSize=sizeof(event);event.unitType=4;event.classId=42;event.unitId=17;
    event.nativeUnit=unitBytes.data();event.source="Gem";event.sourceLength=3;
    char replacement[128]{};
    OnCompatibleInWorldLabel(&event,nullptr);
    Check(observedLabels==1 && positionReads==0,"compat identity observer preserves observation without premature position reads");
    (void)OnCompatibleInWorldStyle(&event,replacement,sizeof(replacement),nullptr);
    Check(positionReads==0,"compat no-icon style skips position work");
    quality=7;
    (void)OnCompatibleInWorldStyle(&event,replacement,sizeof(replacement),nullptr);
    Check(positionReads==4 && MinimapProjectionItems[0].hasIcon && MinimapProjectionItems[0].observedTick,"compat icon keeps guarded coordinates");
    auto replacementTable=std::make_shared<FilterRuleTable>();replacementTable->rules={gem};PublishedFilterRules=replacementTable;
    positionReads=0;fixtureQuantity=5;
    Check(OnCompatibleInWorldStyle(&event,replacement,sizeof(replacement),nullptr) && !std::strcmp(replacement,"5x Gem"),"compat quantity remains after rule reload");
    Check(positionReads==0 && !MinimapProjectionItems[0].hasIcon && observedReplacements,"compat reload clears previous icon and preserves replacement observation");
    event.unitType=1;
    Check(!OnCompatibleInWorldStyle(&event,replacement,sizeof(replacement),nullptr) && positionReads==0,"compat event identity gate retained");
    std::printf("%u actual name-hook/minimap checks; 1800 ordinary labels skip 7200 position reads; icon and mutation paths retained.\n",checks);
}
