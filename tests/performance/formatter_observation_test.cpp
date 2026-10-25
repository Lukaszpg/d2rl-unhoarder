// Actual formatter -> guarded visibility -> presentation identity call chain.
// Own all item/record storage. Native socket execution is a counted stub; the
// production double property reader and ordered rule evaluation are compiled.
#include "ground_quantity_label.hpp"
#include "controller_ground_label.hpp"
#include "minimap_overlay_renderer.hpp"
#include "ground_action_demand.hpp"
#include "ground_property_reader.hpp"
#include "ground_property_live_policy.hpp"
#include "ground_ethereal_policy.hpp"
#include "ground_identified_policy.hpp"
#ifndef FORMATTER_SHARED_QOL
#include "ground_visibility_service.hpp"
#endif
#include "guarded_local_read.hpp"
#include "loot_performance.hpp"
#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <array>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
namespace RuleEngine=UnHoarder::RuleEngine;
namespace GroundPropertyReader=UnHoarder::GroundPropertyReader;
namespace GroundPropertyLive=UnHoarder::GroundPropertyLive;
namespace GroundEthereal=UnHoarder::GroundEthereal;
namespace GroundIdentified=UnHoarder::GroundIdentified;
namespace GroundQuantity=UnHoarder::GroundQuantity;
namespace ControllerGroundLabel=UnHoarder::ControllerGroundLabel;
namespace MinimapOverlayRenderer=UnHoarder::MinimapOverlayRenderer;
namespace MinimapIconPolicy=UnHoarder::MinimapIconPolicy;
namespace Perf=UnHoarder::Perf;
constexpr size_t CandidateTextMaximum=96;
constexpr uintptr_t Base=0x140000000,LabelFormatterCallRva=0x15171E5,LabelFormatterSecondCallRva=0x1517783,DirectCallBytes=5;
#include "label_rule_types.inc"
std::atomic<std::shared_ptr<const FilterRuleTable>> PublishedFilterRules;
std::shared_ptr<FilterRuleTable> fixtureTable;
std::atomic_bool ItemCodeTableReadyMock{true},HideGroundArmed{true},ControllerGroundLabelsReady{true};
enum class GeometryMode {Off,Rules};
std::atomic<GeometryMode> ActiveGeometryMode{GeometryMode::Rules};
thread_local ControllerGroundLabel::Scope ControllerGroundScope;
int ctx{};const int* Context=&ctx;
const char* ResolveBuildName(const int*) {return "93847";}
bool IsSupportedBuild(const char*) {return true;}
constexpr uint32_t Armor=0x00706163;
uint32_t nativeCode=Armor;
uint32_t CanonicalItemCode(uint32_t v){return v;}
bool PrintableItemCode(uint32_t v) noexcept {return v==Armor;}
uint32_t Code(void*){return nativeCode;}
std::uint32_t LookupItemCode(std::uint32_t) noexcept {return Code(nullptr);}
#include "item_code_table.hpp"
namespace ItemCodeTable=UnHoarder::ItemCodeTable;
std::atomic<std::shared_ptr<const UnHoarder::ItemCodeTable::Snapshot>> PublishedItemCodes{};
bool ItemCodeTableReady() noexcept {return ItemCodeTableReadyMock.load();}
std::atomic<void*> GroundQuantityReader;
uint32_t GroundStackQuantity(const void*){return 1;}
unsigned propertyReads{},socketReads{},nativeCalls{},checks{};
bool socketsAvailable=true,doQuery=true;
uint32_t socketCount{};
#include "ground_page_reader.inc"
#include "ground_quality_reader.inc"
bool ReadNativeGroundSockets(const void*,uint32_t,uint32_t,GroundPropertyLive::Purpose,uint32_t& out) noexcept {
    ++socketReads;out=socketCount;return socketsAvailable;
}
bool ReadNativeVendorSellPrice(const void*,uint32_t,uint32_t,uint32_t,GroundPropertyLive::Purpose,uint32_t&,RuleEngine::Item*,uint64_t) noexcept {return false;}
#define GroundRuleItem ProductionGroundRuleItem
#include "ground_rule_item.inc"
#undef GroundRuleItem
RuleEngine::Item GroundRuleItem(uint32_t code,const void* unit,const FilterRuleTable* table,uint32_t id=0,GroundPropertyLive::Purpose purpose=GroundPropertyLive::Purpose::StrictGround,bool geometry=false) noexcept {
    ++propertyReads;return ProductionGroundRuleItem(code,unit,table,id,purpose,geometry);
}
#include "label_rule_resolution.inc"
#include "formatter_identity_state.inc"
#if __has_include("formatter_observation_runtime.inc")
#include "formatter_observation_runtime.inc"
#else
std::atomic<uint64_t> GroundFormatterEpoch{};
#endif
#include "formatter_visibility.inc"
#include "formatter_identity.inc"
alignas(16) std::array<uint8_t,0x40> unitBytes{};
alignas(16) std::array<uint8_t,0x40> data{};
alignas(16) std::array<uint8_t,0x100> recordBytes{};
using VisibilityQuery=UnhoarderVisibility::Decision(__cdecl*)(const void*,uint32_t,uint32_t) noexcept;
VisibilityQuery FixtureVisibilityQuery=&QueryGroundVisibility;
uint8_t __fastcall HookLabelFormatter(void*,void*,void*,uint32_t,uint64_t,uint64_t) noexcept;
unsigned change{};
uintptr_t returnAddress=Base+LabelFormatterCallRva+5;
void* TestReturnAddress(){return reinterpret_cast<void*>(returnAddress);}
uint8_t __fastcall NativeFormatter(void* item,void* dest,void* rec,uint32_t flags,uint64_t a,uint64_t b) noexcept {
    ++nativeCalls;
    std::memcpy(static_cast<char*>(dest)+4,"Cap",4);
    if(doQuery) {
        auto* h=static_cast<uint32_t*>(item);
        (void)FixtureVisibilityQuery(item,h[2],nativeCode);
    }
    if(change==1)unitBytes[8]^=1;
    if(change==2)unitBytes[12]=5;
    if(change==3)unitBytes[0x10]^=8;
    if(change==4)PublishedFilterRules.store(std::make_shared<FilterRuleTable>(*PublishedFilterRules.load()));
    if(change==5)HideGroundArmed=false;
    if(change==6)ActiveGeometryMode=GeometryMode::Off;
    if(change==7)nativeCode=0;
    if(change==10)(void)QueryGroundVisibility(item,0,nativeCode);
    if(change==8)GroundFormatterEpoch.fetch_add(1);
    if(change==9) {
        change=0;const auto previousCaller=returnAddress;returnAddress=Base+123;
        (void)HookLabelFormatter(item,dest,rec,flags,a,b);returnAddress=previousCaller;
    }
    (void)rec;(void)flags;(void)a;(void)b;return 1;
}
auto OriginalLabelFormatter=&NativeFormatter;
#define _ReturnAddress TestReturnAddress
#include "formatter_runtime.inc"
#undef _ReturnAddress
void Check(bool ok,const char* message){++checks;if(!ok){std::fprintf(stderr,"FAIL %s properties=%u sockets=%u\n",message,propertyReads,socketReads);std::exit(1);}}
void Reset() {
    PublishedFilterRules=fixtureTable;
    unitBytes={};recordBytes={};data={};nativeCode=Armor;change=0;doQuery=true;socketsAvailable=true;socketCount=0;
    propertyReads=socketReads=nativeCalls=0;GroundIdentities={};
    auto* h=reinterpret_cast<uint32_t*>(unitBytes.data());h[0]=4;h[1]=42;h[2]=17;h[3]=3;
    auto* d=data.data();std::memcpy(unitBytes.data()+0x10,&d,8);data[0]=2;data[0x38]=99;
    recordBytes[0x10]=17;HideGroundArmed=true;ItemCodeTableReadyMock=true;ActiveGeometryMode=GeometryMode::Rules;
    {
        auto c=std::make_shared<UnHoarder::ItemCodeTable::Snapshot>();
        c->codes.resize(43);c->codes[42]=Armor;
        PublishedItemCodes.store(c);
    }
    returnAddress=Base+LabelFormatterCallRva+5;
}
void Label(){Check(HookLabelFormatter(unitBytes.data(),recordBytes.data()+0x24,recordBytes.data(),0,1,2)==1,"native result preserved");}
int main(int argc,char**) {
    auto table=fixtureTable=std::make_shared<FilterRuleTable>();table->generation=7;table->hiddenRules=1;
    table->usesQuality=table->usesEthereal=table->usesSockets=true;
    FilterNameRule unique{};unique.usesConditions=true;unique.conditions.qualities={7};unique.hasBackground=true;
    FilterNameRule eth{};eth.usesConditions=true;eth.conditions.etherealEnabled=true;eth.conditions.etherealExpected=true;
    FilterNameRule socket{};socket.usesConditions=true;socket.conditions.sockets.enabled=true;socket.conditions.sockets.hasMin=true;socket.conditions.sockets.min=1;
    FilterNameRule hide{};hide.usesConditions=true;hide.show=false;
    table->rules={unique,eth,socket,hide};PublishedFilterRules=table;
    Reset();for(unsigned i=0;i<1800;++i)Label();
    std::printf("Actual formatter/visibility/identity: labels=1800 properties=%u sockets=%u nativeCalls=%u\n",propertyReads,socketReads,nativeCalls);
    if(argc>1)return 0; // recordBytes baseline separately from regression verdict
    Check(propertyReads==1800 && socketReads==1800,"one guarded evaluation supplies pickup query and same-call presentation");
    Check(GroundIdentities[17].unitId==17 && GroundIdentities[17].socketsKnown && GroundIdentities[17].sockets==0,"presentation keeps qualified scalar identity");
    Reset();doQuery=false;Label();Check(propertyReads==1 && socketReads==1,"provider absent retains complete independent presentation query");
    Reset();Label();(void)QueryGroundVisibility(unitBytes.data(),17,Armor);
    Check(propertyReads==2 && socketReads==2,"later pickup query ALWAYS reads properties again");
    Reset();Label();RememberGroundIdentity(unitBytes.data(),recordBytes.data(),true,true,1);
    Check(propertyReads==2 && socketReads==2,"observation cannot survive formatter return");
    for(unsigned mutation=1;mutation<=10;++mutation){Reset();change=mutation;Label();
        Check(propertyReads!=1 || !GroundIdentities[17].unitId,"changed identity/pointer/mode/rules/arming/code refuses handoff");}
    Reset();table->usesQuantity=true;Label();Check(propertyReads==2,"quantity-dependent rules retain separate observation");table->usesQuantity=false;
    Reset();table->usesSellPrice=true;Label();Check(propertyReads==2,"vendor-dependent rules retain separate observation");table->usesSellPrice=false;
    Reset();unitBytes[12]=5;Label();Check(propertyReads==1,"mode5 presentation retained; strict pickup rejects without handoff");
    Reset();socketsAvailable=false;Label();Check(!GroundIdentities[17].unitId,"unavailable socket value remains visible without hidden identity");
    Reset();returnAddress=Base+123;Label();Check(propertyReads==1 && !GroundIdentities[17].unitId,"unqualified formatter does not publish presentation identity");
    Reset();returnAddress=Base+ControllerGroundLabel::FormatterReturn;Label();Check(propertyReads==1 && GroundIdentities[17].unitId==17,"controller paired output uses same guarded observation");
    std::printf("%u formatter handoff/fresh-pickup/fallback checks passed.\n",checks);
    return 0;
}
