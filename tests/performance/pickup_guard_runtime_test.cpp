#include <windows.h>
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "native_pickup_guard_policy.hpp"
#include "native_pickup_profile.hpp"
#include "loot_performance.hpp"
using namespace UnHoarder;
struct Rules {unsigned hiddenRules=1;};
std::atomic<std::shared_ptr<const Rules>> PublishedFilterRules{std::make_shared<Rules>()};
enum class GeometryMode {Rules,Other};
std::atomic<GeometryMode> ActiveGeometryMode{GeometryMode::Rules};
std::atomic_bool HideGroundArmed{true},ItemCodeReaderReady{true};
std::array<uint32_t,4> item{4,11,42,3};
uint32_t playerStorage{},inputMode{};
uintptr_t manager=0x12340000,Base{};
short leftAlt{},rightAlt{};
unsigned lookedUp{},resolved{},forwarded{},invalidated{},checks{},keyReads{};
bool missing{},badCode{},visible{},unknown{},mutate{},failRead{},switchMode{},lateAlt{};
bool ReadSafe(uintptr_t rva,void* out,size_t bytes) noexcept {
    if(rva!=NativePickupProfile::ManagerSlotRva || bytes!=8)return false;
    std::memcpy(out,&manager,8);return true;
}
BOOL FixtureRead(HANDLE,const void* source,void* out,SIZE_T size,SIZE_T* copied) noexcept {
    *copied=0;if(failRead)return FALSE;
    if(reinterpret_cast<uintptr_t>(source)==manager+0xdc && size==4)std::memcpy(out,&inputMode,4);
    else if(source==&playerStorage && size==4)std::memcpy(out,&playerStorage,4);
    else if(source==item.data() && size==16)std::memcpy(out,item.data(),16);
    else return FALSE;
    *copied=size;return TRUE;
}
short FixtureKey(int key) noexcept {++keyReads;return key==VK_LMENU?leftAlt:rightAlt;}
void* __fastcall Lookup(uint32_t id,uint32_t type) noexcept {
    ++lookedUp;return !missing && id==42 && type==4?item.data():nullptr;
}
uint32_t Code(void*) noexcept {return badCode?0:0x6d6567;}
auto NativeGetItemCode=&Code;
uint32_t CanonicalItemCode(uint32_t code) noexcept {return code;}
bool PrintableItemCode(uint32_t code) noexcept {return code!=0;}
int GroundRuleItem(uint32_t,void*,const Rules*,uint32_t) noexcept {
    if(mutate)++item[2];if(switchMode)inputMode=1;if(lateAlt)leftAlt=static_cast<short>(0x8000);return 0;
}
struct GroundRuleDecision {bool show;};
bool ResolveGroundRule(const Rules*,int,GroundRuleDecision& result) noexcept {
    ++resolved;result.show=visible;return !unknown;
}
void ForgetVendorSellPriceItem(uint32_t) noexcept {++invalidated;}
#define ReadProcessMemory FixtureRead
#define GetAsyncKeyState FixtureKey
#include "pickup_guard_runtime.inc"
#undef GetAsyncKeyState
#undef ReadProcessMemory
void __fastcall Forward(uint32_t,void*,uint32_t,uint32_t) noexcept {++forwarded;}
void Check(bool ok,const char* label) {++checks;if(!ok){std::printf("FAIL %s\n",label);std::exit(1);}}
void Reset() {
    item={4,11,42,3};playerStorage=0;inputMode=0;manager=0x12340000;
    leftAlt=rightAlt=0;lookedUp=resolved=forwarded=invalidated=keyReads=0;
    missing=badCode=visible=unknown=mutate=failRead=switchMode=lateAlt=false;
    HideGroundArmed=true;ItemCodeReaderReady=true;ActiveGeometryMode=GeometryMode::Rules;
    PublishedFilterRules=std::make_shared<Rules>();NativePickupGuardQualified=true;
    Base=reinterpret_cast<uintptr_t>(&Lookup)-NativeItemLookupRva;
    OriginalNativeActionDispatch=&Forward;NativeGetItemCode=&Code;
}
void Pickup() {HookNativeActionDispatch(22,&playerStorage,4,42);}
int main() {
    Reset();Pickup();Check(forwarded==0 && lookedUp==1 && resolved==1,"hidden gear-icon pickup blocked without any label state");
    for(bool right:{false,true}) {Reset();(right?rightAlt:leftAlt)=static_cast<short>(0x8000);Pickup();Check(forwarded==1 && invalidated==1 && lookedUp==0,"either held ALT permits once without property work");}
    Reset();leftAlt=rightAlt=1;Pickup();Check(forwarded==0,"historical key-press low bit is not a held override");
    Reset();leftAlt=static_cast<short>(0x8000);Pickup();leftAlt=0;Pickup();Check(forwarded==1 && lookedUp==1,"release immediately restores block without a timer");
    Reset();lateAlt=true;Pickup();Check(forwarded==1,"ALT pressed during evaluation permits pickup");
    Reset();visible=true;Pickup();Check(forwarded==1 && invalidated==1,"visible item forwarded once");
    Reset();unknown=true;Pickup();Check(forwarded==1,"unknown rule/property result fails open");
    Reset();mutate=true;Pickup();Check(forwarded==1,"recycled unit identity fails open");
    Reset();switchMode=true;Pickup();Check(forwarded==1,"input-mode transition fails open");
    for(uint32_t mode:{1u,2u,~0u}) {Reset();inputMode=mode;Pickup();Check(forwarded==1 && lookedUp==0,"controller/unknown input retains native/QOL policy");}
    Reset();manager=0;Pickup();Check(forwarded==1 && lookedUp==0,"missing UI manager fails open");
    Reset();NativePickupGuardQualified=false;Pickup();Check(forwarded==1 && keyReads==0,"unqualified guard forwards without reads");
    Reset();HideGroundArmed=false;Pickup();Check(forwarded==1 && lookedUp==0,"inactive filter forwards");
    Reset();PublishedFilterRules=std::make_shared<Rules>(Rules{0});Pickup();Check(forwarded==1 && lookedUp==0,"no hidden rules avoids properties");
    Reset();missing=true;Pickup();Check(forwarded==1,"lookup failure forwards");
    Reset();badCode=true;Pickup();Check(forwarded==1,"invalid code forwards");
    Reset();failRead=true;Pickup();Check(forwarded==1,"unreadable state forwards");
    for(uint32_t mode:{0u,5u}) {Reset();item[3]=mode;Pickup();Check(forwarded==1,"non-ground/presentation-only mode cannot suppress pickup");}
    Reset();item[2]=43;Pickup();Check(forwarded==1,"lookup identity mismatch forwards");
    Reset();playerStorage=1;Pickup();Check(forwarded==1,"non-player actor fails open");
    Reset();HookNativeActionDispatch(21,&playerStorage,4,42);Check(forwarded==1 && invalidated==0 && keyReads==0,"non-pickup has no additional work");
    Reset();HookNativeActionDispatch(22,&playerStorage,2,42);Check(forwarded==1 && invalidated==0 && keyReads==0,"non-item action untouched");
    std::printf("%u actual pickup callback checks passed\n",checks);
}
