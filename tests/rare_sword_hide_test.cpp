#include "src/filter_rule_engine.hpp"
#include <cassert>
#include <cstdio>
using namespace UnHoarder::RuleEngine;
struct Rule {bool usesConditions=true;uint32_t code{};Conditions conditions{};bool show=true;};
int main(){
    Rule socketed{},hidden{},fallback{};
    socketed.conditions.sockets={true,false,true,false,0,1,0};
    hidden.show=false;hidden.conditions.typeCodes={0x00777362}; // canonical "bsw"
    hidden.conditions.qualities={1,2,3,4,6};
    hidden.conditions.sockets={true,true,false,false,0,0,0};
    hidden.conditions.etherealEnabled=true;hidden.conditions.etherealExpected=false;
    std::vector<Rule> rules{socketed,hidden,fallback};
    Item sword{};sword.code=0x00777362;sword.qualityKnown=true;sword.quality=6;
    sword.etherealKnown=true;sword.ethereal=false;
    assert(!FirstMatchFailOpenProperties(rules,sword)); // actual failure: unknown socket count stops before hide
    sword.socketsKnown=true;sword.sockets=0;
    assert(FirstMatchFailOpenProperties(rules,sword)==&rules[1]);
    sword.sockets=1;assert(FirstMatchFailOpenProperties(rules,sword)==&rules[0]);
    sword.sockets=0;sword.ethereal=true;assert(FirstMatchFailOpenProperties(rules,sword)==&rules[2]);
    sword.etherealKnown=false;assert(!FirstMatchFailOpenProperties(rules,sword));
    std::puts("Rare sword: unknown sockets fail open; proven zero sockets reaches hide; socketed or ethereal items retain show behavior.");
}
