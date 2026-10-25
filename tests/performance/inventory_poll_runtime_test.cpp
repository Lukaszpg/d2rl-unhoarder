#include <D2RLPlugin/api.h>
#include <D2RLPlugin/inventory.h>
#include "ground_sound_registry.hpp"
#include "loot_performance.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Compile the exact production callbacks. SDK responses are owned copies;
// no DLL, native game function or running game is involved.
namespace Perf=UnHoarder::Perf;
struct FilterRuleTable {bool usesSellPrice{};std::size_t minimapIconRules{},soundRules{};std::vector<int> rules;};
enum class GeometryMode {Off,Rules};
std::atomic<GeometryMode> ActiveGeometryMode{GeometryMode::Rules};
std::atomic<std::shared_ptr<const FilterRuleTable>> PublishedFilterRules{};
std::atomic_bool SoundArmed{},AutomapProjectionHookInstalled{true},SoundPickupPollPending{};
const D2RL::InventoryService* SoundInventory{};
const D2RL::ItemService* SoundInventoryItems{};
UnHoarder::SoundIdentity::Registry SoundSeenRegistry;
std::mutex SoundSeenMutex;
std::vector<unsigned> minimapForgot,priceForgot;
unsigned enumerations{},cursorReads{},mask{},checks{};
bool playerReady=true,cursorReady=true,cursorIdentityValid=true;
std::vector<std::string> reported;
unsigned consoleLines{};
void __cdecl ReportLog(const D2RL::PluginContext*,const char* text) noexcept {reported.emplace_back(text);}
void __cdecl ReportConsole(const D2RL::PluginContext*,const char*,D2RL::ConsoleMessageKind) noexcept {++consoleLines;}
void LogInfo(const char*) noexcept {}
void ForgetMinimapProjectionItem(unsigned id) noexcept {minimapForgot.push_back(id);}
void ForgetVendorSellPriceItem(unsigned id) noexcept {priceForgot.push_back(id);}
#include "inventory_poll.inc"
const D2RL::PluginContext* Context{};
#include "performance_command.inc"

void Check(bool ok,const char* why) {
    ++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}
}
D2RL::Inventory::Result __cdecl Player(const D2RL::PluginContext*,D2RL::PlayerHandle* out) noexcept {
    *out=1;return playerReady?D2RL::Inventory::Result::Success:D2RL::Inventory::Result::Unavailable;
}
D2RL::Inventory::Result __cdecl Enumerate(const D2RL::PluginContext* context,D2RL::PlayerHandle,
    const D2RL::Inventory::ItemFilter* filter,D2RL::Inventory::ItemCallback callback,void* user) noexcept {
    ++enumerations;mask=filter->containerMask;
    // One charm/custom-page item, one normal inventory item and ignored ground
    // and malformed copies test the production callback's scope guards.
    D2RL::Items::ItemInfo item{};item.structSize=D2RL::Items::ItemInfoSize;item.runtimeId=17;
    item.container=D2RL::Items::ItemContainer::CustomPage;(void)callback(context,&item,user);
    item.runtimeId=19;item.container=D2RL::Items::ItemContainer::Inventory;(void)callback(context,&item,user);
    item.runtimeId=21;item.container=D2RL::Items::ItemContainer::Ground;(void)callback(context,&item,user);
    item.runtimeId=23;item.structSize=0;(void)callback(context,&item,user);
    return D2RL::Inventory::Result::Success;
}
D2RL::Inventory::Result __cdecl Cursor(const D2RL::PluginContext*,D2RL::PlayerHandle,D2RL::ItemHandle* out) noexcept {
    ++cursorReads;*out=33;return cursorReady?D2RL::Inventory::Result::Success:D2RL::Inventory::Result::Unavailable;
}
D2RL::Items::Result __cdecl CursorInfo(const D2RL::PluginContext*,D2RL::ItemHandle,D2RL::Items::ItemInfo* out) noexcept {
    out->structSize=cursorIdentityValid?D2RL::Items::ItemInfoSize:0;
    out->runtimeId=33;out->container=D2RL::Items::ItemContainer::Cursor;return D2RL::Items::Result::Success;
}
void Reset() {
    enumerations=cursorReads=mask=0;minimapForgot.clear();priceForgot.clear();SoundSeenRegistry.Clear();
    for(auto id:{17u,19u,21u,23u,33u})Check(SoundSeenRegistry.Observe(id,0x206e6972)==
        UnHoarder::SoundIdentity::Registry::Observation::New,"seed sound registry");
}
int main() {
    D2RL::PluginContext context{};D2RL::InventoryService inventory{};D2RL::ItemService items{};
    inventory.getLocalPlayer=&Player;inventory.forEachInventoryItem=&Enumerate;inventory.getCursorItem=&Cursor;
    items.getItemInfo=&CursorInfo;SoundInventory=&inventory;SoundInventoryItems=&items;
    auto poll=[&] {SoundPickupPollPending=true;PollSoundInventoryOnUiThread(&context,nullptr);
        Check(!SoundPickupPollPending,"pending token releases on every exit");};
    Reset();poll();Check(enumerations==0 && cursorReads==0,"no filter does not enumerate inventory or cursor");
    Check(Perf::Counters[Perf::Enumeration].Read().calls==0 && Perf::Counters[Perf::Poll].Read().calls==0,
        "no-filter report confirms zero polling work");
    auto rules=std::make_shared<FilterRuleTable>();PublishedFilterRules.store(rules);
    Reset();poll();Check(enumerations==0,"text-only rules do not enumerate storage");
    for(unsigned features=1;features<8;++features) {
        rules=std::make_shared<FilterRuleTable>();rules->minimapIconRules=(features&2)?1u:0u;
        rules->soundRules=(features&1)?1u:0u;rules->usesSellPrice=(features&4)!=0;
        PublishedFilterRules.store(rules);SoundArmed=(features&1)!=0;Reset();poll();
        Check(enumerations==1 && cursorReads==1,"active feature retains inventory and cursor observation");
        Check(Perf::Counters[Perf::Enumeration].Read().calls==features &&
            Perf::Counters[Perf::Enumeration].Read().units==features*2 &&
            Perf::Counters[Perf::Poll].Read().units==features*3,
            "production metrics count actual enumeration and include cursor only in total poll items");
        const std::vector<unsigned> ids{17,19,33};
        Check(minimapForgot==((features&2)?ids:std::vector<unsigned>{}),"only active minimap invalidates its observations");
        Check(priceForgot==((features&4)?ids:std::vector<unsigned>{}),"only active pricing invalidates its cache");
        Check(mask==(D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory)|
            D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Equipment)|
            D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Belt)|
            D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Cube)|
            D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::PersonalStash)|
            D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::SharedStash)|
            D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::CustomPage)),"container coverage unchanged");
        for(auto id:ids)Check(SoundSeenRegistry.Observe(id,0x206e6972)==((features&1)?
            UnHoarder::SoundIdentity::Registry::Observation::New:UnHoarder::SoundIdentity::Registry::Observation::AlreadySeen),
            "only armed sound observation re-arms inventory/cursor IDs");
        for(auto id:{21u,23u})Check(SoundSeenRegistry.Observe(id,0x206e6972)==
            UnHoarder::SoundIdentity::Registry::Observation::AlreadySeen,"ground and malformed copies never re-arm");
    }
    playerReady=false;Reset();poll();Check(enumerations==0,"unavailable local player is skipped");playerReady=true;
    cursorIdentityValid=false;Reset();poll();Check(minimapForgot==std::vector<unsigned>({17,19}),"malformed cursor snapshot ignored");cursorIdentityValid=true;
    PublishedFilterRules.store({});SoundArmed=false;Reset();poll();Check(enumerations==0,"queued callback rechecks filter demand");
    rules=std::make_shared<FilterRuleTable>();rules->minimapIconRules=1;PublishedFilterRules.store(rules);
    AutomapProjectionHookInstalled=false;Reset();poll();Check(enumerations==0,"unavailable projection does not cause icon polling");
    AutomapProjectionHookInstalled=true;ActiveGeometryMode=GeometryMode::Off;Reset();poll();
    Check(enumerations==0,"inactive native filtering does not enumerate storage");
    D2RL::PluginApi api{};api.apiSize=sizeof(api);api.logInfo=&ReportLog;api.writeConsoleMessage=&ReportConsole;
    context.contextSize=sizeof(context);context.api=&api;
    D2RL::ConsoleCommandContext command{};command.plugin=&context;command.args=" perf ";command.argsLength=6;
    const auto previousRules=PublishedFilterRules.load();
    const auto previousCalls=Perf::Counters[Perf::Poll].Read().calls;
    Check(PerformanceCommand(nullptr,&command,nullptr)==D2RL::ConsoleCommandResult::Handled &&
        reported.size()==Perf::Count+3 && consoleLines==reported.size(),"performance command writes bounded console and log report");
    Check(reported.back().find("PAGE_QUERY owner=unhoarder")!=std::string::npos &&
        reported.back().find("fresh-per-read=1")!=std::string::npos,"performance command includes read-only page-path counters");
    Check(PublishedFilterRules.load()==previousRules && ActiveGeometryMode==GeometryMode::Off &&
        Perf::Counters[Perf::Poll].Read().calls==previousCalls,"read-only report changes no filter state or producer counters");
    command.args="perf on";command.argsLength=7;
    Check(PerformanceCommand(nullptr,&command,nullptr)==D2RL::ConsoleCommandResult::InvalidArguments,
        "performance command has no hidden settings toggle");
    std::printf("%u production inventory/cursor lifecycle checks passed.\n",checks);
}
