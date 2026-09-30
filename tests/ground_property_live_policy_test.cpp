#include "../src/ground_property_live_policy.hpp"
#include "../src/filter_rule_engine.hpp"
#include <cassert>
#include <vector>
using namespace UnHoarder;
struct TestRule {bool schema2{true}; std::uint32_t code{}; RuleEngine::Conditions conditions{};bool show{true};};
int main() {
    using namespace GroundPropertyLive;
    auto a=Validate(3,84);assert(a.qualityKnown&&a.itemLevelKnown&&a.quality==3&&a.itemLevel==84);
    auto b=Validate(6,99);assert(b.qualityKnown&&b.itemLevelKnown&&b.quality==6&&b.itemLevel==99);
    assert(!Validate(0,85).qualityKnown);
    assert(!Validate(10,85).qualityKnown);
    assert(!Validate(4,0).itemLevelKnown);
    assert(!Validate(4,100).itemLevelKnown);
    assert(!Ready(true,true,false,true));
    assert(!Ready(true,false,false,true));
    assert(!Ready(false,true,true,false));
    assert(Ready(true,true,true,true));
    assert(Ready(false,false,false,false));
    assert(AllowsMode(3,Purpose::StrictGround));
    assert(!AllowsMode(5,Purpose::StrictGround));
    assert(!AllowsMode(4,Purpose::StrictGround));
    assert(AllowsMode(3,Purpose::VerifiedLabel));
    assert(AllowsMode(5,Purpose::VerifiedLabel));
    assert(!AllowsMode(4,Purpose::VerifiedLabel));
    RuleEngine::Item item{};item.code=101;item.qualityKnown=true;
    item.quality=6;item.itemLevelKnown=true;item.itemLevel=84;
    TestRule show{};show.conditions.codes={101};show.conditions.qualities={6};
    show.conditions.itemLevel.enabled=true;
    show.conditions.itemLevel.hasMin=true;show.conditions.itemLevel.min=85;
    TestRule hide{};hide.show=false;hide.conditions.codes={101};
    std::vector<TestRule> rules{show,hide};
    assert(RuleEngine::FirstMatch(rules,item)==&rules[1]);
    item.itemLevel=85;
    assert(RuleEngine::FirstMatch(rules,item)==&rules[0]);
    item.qualityKnown=false;
    // Raw portable first-match permits generic fallback, so the shared
    // native caller MUST check Ready() before calling FirstMatch().
    assert(!Ready(true,true,item.qualityKnown,item.itemLevelKnown));
    item.qualityKnown=true;item.quality=4;
    assert(RuleEngine::FirstMatch(rules,item)==&rules[1]);
}
