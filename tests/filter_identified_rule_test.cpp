#include "../src/filter_rule_engine.hpp"
#include "../src/ground_identified_policy.hpp"
#include "../src/ground_property_live_policy.hpp"
#include <cassert>
#include <vector>
using namespace SoE::LootFilter;
using namespace SoE::LootFilter::RuleEngine;
struct Rule {bool schema2{true}; std::uint32_t code{}; Conditions conditions{}; bool show{true};};
int main() {
    static_assert(GroundIdentified::NativeIdentifiedMask==0x00000010U);
    static_assert(!GroundIdentified::FromNativeFlags(0x10800000U));
    static_assert(GroundIdentified::FromNativeFlags(0x00800010U));
    static_assert(GroundPropertyLive::AllowsMode(5,GroundPropertyLive::Purpose::VerifiedLabel));
    static_assert(!GroundPropertyLive::AllowsMode(5,GroundPropertyLive::Purpose::StrictGround));
    Rule code{}; code.conditions.codes={1};
    Rule unid{}; unid.conditions.baseCodes={2};
    unid.conditions.identifiedEnabled=true;unid.conditions.identifiedExpected=false;
    Rule broadHide{};broadHide.show=false;
    std::vector<Rule> rules{code,unid,broadHide};
    Item item{};item.code=1;
    assert(NextNativeProperty(rules,item)==NextProperty::None); // no unnecessary reader
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[0]);
    item.code=2;
    assert(NextNativeProperty(rules,item)==NextProperty::Identified);
    assert(!FirstMatchFailOpenProperties(rules,item)); // unknown != unid; don't hide
    item.identifiedKnown=true;item.identified=false;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
    item.identified=true;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
    unid.conditions.identifiedExpected=true;rules={code,unid,broadHide};
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
    item.identifiedKnown=false;
    assert(!FirstMatchFailOpenProperties(rules,item)); // unknown != identified
    // Known mismatch skips a rule even when another field is unknown.
    Rule combo=unid;combo.conditions.etherealEnabled=true;
    combo.conditions.etherealExpected=true;
    rules={code,combo,broadHide};
    item.identifiedKnown=true;item.identified=true;
    item.etherealKnown=false;
    assert(NextNativeProperty(rules,item)==NextProperty::Ethereal);
    item.etherealKnown=true;item.ethereal=false;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
    item.ethereal=true;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
    item.identified=false;
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
    item.identified=true;
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
    // Combined quality/socket/flag rule requires properties in staged order.
    combo.conditions.qualities={7};
    combo.conditions.sockets.enabled=true;
    combo.conditions.sockets.hasEq=true;
    combo.conditions.sockets.eq=4;
    rules={code,combo,broadHide};
    item.qualityKnown=false;item.socketsKnown=false;
    item.etherealKnown=false;item.identifiedKnown=false;
    assert(NextNativeProperty(rules,item)==NextProperty::QualityLevel);
    item.qualityKnown=true;item.quality=7;
    assert(NextNativeProperty(rules,item)==NextProperty::Sockets);
    item.socketsKnown=true;item.sockets=4;
    assert(NextNativeProperty(rules,item)==NextProperty::Ethereal);
    item.etherealKnown=true;item.ethereal=true;
    assert(NextNativeProperty(rules,item)==NextProperty::Identified);
    item.identifiedKnown=true;item.identified=true;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
}
