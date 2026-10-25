#include <windows.h>
#include "ground_action_demand.hpp"
#include "ground_label_visibility_service.h"
#include "loot_performance.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
namespace RuleEngine=UnHoarder::RuleEngine;
namespace Perf=UnHoarder::Perf;
using D=UnhoarderLabelVisibility::Decision;
struct Rule {bool usesConditions{true},continueEvaluation{},show{true};uint32_t code{};RuleEngine::Conditions conditions{};};
struct FilterRuleTable {std::vector<Rule> rules;};
std::atomic<std::shared_ptr<const FilterRuleTable>> PublishedFilterRules;
std::atomic<bool> ItemCodeTableReadyMock{true},HideGroundArmed{true};
enum class GeometryMode {Off,Rules};
std::atomic<GeometryMode> ActiveGeometryMode{GeometryMode::Rules};
constexpr uint32_t Gem=0x00726d67,Armor=0x00706163,Potion=0x00317068;
std::array<uint32_t,4> nativeHeader{4,42,17,3};
unsigned codeCalls{},mutation{},checks{};
uint32_t nativeCode=Gem;
std::shared_ptr<FilterRuleTable> table;
uint32_t CanonicalItemCode(uint32_t code) noexcept {return code&0xffffff;}
bool PrintableItemCode(uint32_t code) noexcept {return code==Gem || code==Armor || code==Potion;}
uint32_t __fastcall ReadCode(void*) noexcept {
    ++codeCalls;
    if(codeCalls==2) {
        if(mutation==1)nativeHeader[2]++;
        if(mutation==2)nativeHeader[1]++;
        if(mutation==3)nativeHeader[3]=5;
        if(mutation==4)return Armor;
        if(mutation==5)PublishedFilterRules.store(std::make_shared<FilterRuleTable>(*table));
        if(mutation==6)HideGroundArmed=false;
        if(mutation==7)ActiveGeometryMode=GeometryMode::Off;
    }
    return nativeCode;
}
#include "item_code_table.hpp"
namespace ItemCodeTable=UnHoarder::ItemCodeTable;
std::atomic<std::shared_ptr<const UnHoarder::ItemCodeTable::Snapshot>> PublishedItemCodes{};
std::uint32_t LookupItemCode(std::uint32_t) noexcept {return ReadCode(nullptr);}
bool ItemCodeTableReady() noexcept {return ItemCodeTableReadyMock.load();}
#include "label_preflight_runtime.inc"
static void Check(bool ok,const char* what){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}}
static void Reset(){nativeHeader={4,42,17,3};nativeCode=Gem;mutation=codeCalls=0;ItemCodeTableReadyMock=true;HideGroundArmed=true;ActiveGeometryMode=GeometryMode::Rules;PublishedFilterRules.store(table);auto c=std::make_shared<UnHoarder::ItemCodeTable::Snapshot>();c->codes.resize(43);c->codes[42]=Gem;PublishedItemCodes.store(c);}
int main() {
    Rule unique;unique.conditions.qualities={7};
    Rule hide;hide.show=false;hide.conditions.typeCodes={Armor};hide.conditions.qualities={1,2,3,4,6};hide.conditions.sockets={true,true,false,false,0};
    Rule potion; potion.show=false;potion.conditions.codes={Potion};
    table=std::make_shared<FilterRuleTable>();table->rules={unique,hide,potion};Reset();
    for(unsigned frame=0;frame<120;++frame)for(uint32_t id=1;id<=15;++id){nativeHeader[2]=id;Check(QueryLabelCannotHide(nativeHeader.data(),id)==D::CannotHide,"town gem cannot match equipment/potion Hide rules");}
    Check(codeCalls==0,"all gem lookups use immutable SDK table without native code calls");
    Reset();{auto c=std::make_shared<UnHoarder::ItemCodeTable::Snapshot>();c->codes.resize(43);c->codes[42]=Armor;PublishedItemCodes.store(c);}Check(QueryLabelCannotHide(nativeHeader.data(),17)==D::RequireQuery,"unknown rarity/socket values retain original full query");
    Reset();{auto c=std::make_shared<UnHoarder::ItemCodeTable::Snapshot>();c->codes.resize(43);c->codes[42]=Potion;PublishedItemCodes.store(c);}Check(QueryLabelCannotHide(nativeHeader.data(),17)==D::RequireQuery,"known hidden code retains full query");
    Reset();PublishedItemCodes.store({});Check(QueryLabelCannotHide(nativeHeader.data(),17)==D::RequireQuery,"invalid or unloaded SDK item code snapshot cannot skip query");
    Reset();nativeHeader[0]=0;Check(QueryLabelCannotHide(nativeHeader.data(),17)==D::RequireQuery,"non-item refused");
    Reset();nativeHeader[2]=18;Check(QueryLabelCannotHide(nativeHeader.data(),17)==D::RequireQuery,"wrong id refused");
    Reset();nativeHeader[3]=5;Check(QueryLabelCannotHide(nativeHeader.data(),17)==D::RequireQuery,"strict ground mode retained");
    Reset();{auto c=std::make_shared<UnHoarder::ItemCodeTable::Snapshot>();c->codes.resize(43);PublishedItemCodes.store(c);}Check(QueryLabelCannotHide(nativeHeader.data(),17)==D::RequireQuery,"invalid code refused");
    Reset();ItemCodeTableReadyMock=false;Check(QueryLabelCannotHide(nativeHeader.data(),17)==D::RequireQuery,"unavailable code reader refused");
    Reset();PublishedFilterRules.store(nullptr);Check(QueryLabelCannotHide(nativeHeader.data(),17)==D::RequireQuery,"missing rules cannot skip query");
    Reset();Check(QueryLabelCannotHide(nullptr,17)==D::RequireQuery && QueryLabelCannotHide(nativeHeader.data(),0)==D::RequireQuery,"invalid borrowed observation refused");
    // A positive proof must imply no Hidden result for ALL completions of the
    // unknown properties, including Continue and unavailable native fields.
    unsigned parity{};
    for(uint32_t code:{Gem,Armor,Potion})
    for(unsigned order=0;order<2;++order)
    for(unsigned continuation=0;continuation<8;++continuation)
    for(unsigned quality=0;quality<=9;++quality)
    for(unsigned sockets=0;sockets<4;++sockets)
    for(unsigned flags=0;flags<4;++flags) {
        auto rules=table->rules;if(order)std::reverse(rules.begin(),rules.end());
        for(unsigned i=0;i<rules.size();++i)rules[i].continueEvaluation=(continuation&(1u<<i))!=0;
        RuleEngine::Item partial{};partial.code=code;
        auto complete=partial;complete.qualityKnown=quality!=0;complete.quality=quality;complete.socketsKnown=sockets!=3;complete.sockets=sockets;
        complete.etherealKnown=(flags&1)!=0;complete.ethereal=(flags&2)!=0;
        bool show=true;
        RuleEngine::ResolveMatchingRulesFailOpen(rules,complete,[&](const Rule& rule){show=rule.show;},[&]{show=true;});
        Check(UnHoarder::GroundActionDemand::HidePossible(rules,partial) || show,"no-Hide proof never skips a potentially hidden completion");++parity;
    }
    Rule unknown;unknown.show=false;unknown.conditions.etherealEnabled=true;
    RuleEngine::Item partial{};partial.code=Gem;
    Check(UnHoarder::GroundActionDemand::HidePossible(std::vector<Rule>{unknown},partial),"unknown hide predicate is never treated as false");
    std::printf("%u preflight guard checks; %u completed-rule parity cases; 1800 gem observations use no property reader.\n",checks,parity);
    return 0;
}
