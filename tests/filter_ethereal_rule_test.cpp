#include "../src/filter_rule_engine.hpp"
#include "../src/ground_ethereal_policy.hpp"
#include "../src/ground_property_live_policy.hpp"
#include <cassert>
#include <vector>
using namespace UnHoarder;
using namespace UnHoarder::RuleEngine;
struct Rule {bool usesConditions{true};std::uint32_t code{};Conditions conditions{};bool show{true};};
int main() {
    static_assert(GroundEthereal::NativeEtherealMask==0x00400000U);
    static_assert(GroundEthereal::FromNativeFlags(0x00C02010U));
    static_assert(!GroundEthereal::FromNativeFlags(0x00800010U));
    static_assert(GroundPropertyLive::AllowsMode(5,GroundPropertyLive::Purpose::VerifiedLabel));
    static_assert(!GroundPropertyLive::AllowsMode(5,GroundPropertyLive::Purpose::StrictGround));
    Rule code{};code.conditions.codes={1};
    Rule eth{};eth.conditions.baseCodes={2};eth.conditions.etherealEnabled=true;
    eth.conditions.etherealExpected=true;
    Rule broadHide{};broadHide.show=false;
    std::vector<Rule> rules{code,eth,broadHide};
    Item item{};item.code=1;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[0]);
    item.code=2;
    assert(NextNativeProperty(rules,item)==NextProperty::Ethereal);
    assert(!FirstMatchFailOpenProperties(rules,item)); // unknown != false
    item.etherealKnown=true;item.ethereal=true;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
    item.ethereal=false;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
    eth.conditions.etherealExpected=false;rules={code,eth,broadHide};
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
    item.etherealKnown=false;
    assert(!FirstMatchFailOpenProperties(rules,item));
    // Known eth mismatch skips the entire rule even if its quality is missing.
    Rule combo=eth;combo.conditions.etherealExpected=true;
    combo.conditions.qualities={7};rules={code,combo,broadHide};
    item.etherealKnown=true;item.ethereal=false;item.qualityKnown=false;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
    item.ethereal=true;
    assert(NextNativeProperty(rules,item)==NextProperty::QualityLevel);
    assert(!FirstMatchFailOpenProperties(rules,item));
    item.qualityKnown=true;item.quality=7;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
    // A known socket mismatch avoids an otherwise needed flag read.
    combo.conditions.sockets.enabled=true;combo.conditions.sockets.hasMin=true;
    combo.conditions.sockets.min=4;rules={code,combo,broadHide};
    item.etherealKnown=false;item.socketsKnown=true;item.sockets=3;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
    item.sockets=4;
    assert(NextNativeProperty(rules,item)==NextProperty::Ethereal);
    assert(!FirstMatchFailOpenProperties(rules,item));
}
