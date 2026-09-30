#include "../src/filter_rule_engine.hpp"
#include <cassert>
#include <vector>
using namespace UnHoarder::RuleEngine;
struct Rule { bool schema2{true}; std::uint32_t code{}; Conditions conditions{}; bool show{}; };
int main() {
    Rule show{};show.conditions.codes={123};show.conditions.qualities={3,6};
    show.conditions.itemLevel.enabled=true;
    show.conditions.itemLevel.hasMin=true;show.conditions.itemLevel.min=85;
    show.conditions.sockets.enabled=true;
    show.conditions.sockets.hasEq=true;show.conditions.sockets.eq=4;
    show.conditions.etherealEnabled=true;show.conditions.etherealExpected=true;
    show.conditions.identifiedEnabled=true;show.conditions.identifiedExpected=false;
    std::vector<Rule> rules{show};
    Item item{};item.code=123;
    assert(!FirstMatch(rules,item)); // unknown must never match false/zero
    item.qualityKnown=true;item.quality=3;
    assert(!FirstMatch(rules,item));
    item.itemLevelKnown=true;item.itemLevel=86;
    item.socketsKnown=true;item.sockets=4;
    item.etherealKnown=true;item.ethereal=true;
    item.identifiedKnown=true;item.identified=false;
    assert(FirstMatch(rules,item));
    item.identified=true;assert(!FirstMatch(rules,item));
    item.identified=false;item.quality=5;assert(!FirstMatch(rules,item));
    item.quality=6;assert(FirstMatch(rules,item));
    item.itemLevel=84;assert(!FirstMatch(rules,item));
    item.itemLevel=85;assert(FirstMatch(rules,item));
    item.etherealKnown=false;assert(!FirstMatch(rules,item));
    show.conditions.superiorEnabled=true;
    show.conditions.superiorExpected=true;
    rules={show};
    item.etherealKnown=true;item.quality=6;assert(!FirstMatch(rules,item));
    item.quality=3;assert(FirstMatch(rules,item));
    item.qualityKnown=false;assert(!FirstMatch(rules,item));
}
