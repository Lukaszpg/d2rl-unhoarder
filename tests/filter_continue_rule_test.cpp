#include "../src/filter_rule_engine.hpp"
#include <cassert>
#include <vector>
using namespace UnHoarder::RuleEngine;

struct Rule {
    bool usesConditions{true};
    std::uint32_t code{};
    Conditions conditions{};
    bool show{true};
    bool continueEvaluation{};
    int marker{};
};

int main() {
    Rule broad{};
    broad.conditions.codes={1};
    broad.show=true;
    broad.continueEvaluation=true;
    broad.marker=10;

    Rule specific{};
    specific.conditions.codes={1};
    specific.conditions.sockets.enabled=true;
    specific.conditions.sockets.hasMin=true;
    specific.conditions.sockets.min=6;
    specific.show=false;
    specific.marker=20;

    Rule fallback{};
    fallback.show=true;
    fallback.marker=30;

    std::vector<Rule> rules{broad,specific,fallback};
    Item item{};
    item.code=1;

    // The continued first match means the later socket block still matters.
    assert(NextNativeProperty(rules,item)==NextProperty::Sockets);

    // Until sockets are known, fail-open resolution retains actions from the
    // already matched continued block but does not fall through to fallback.
    std::vector<int> applied;
    bool finalShow=false;
    const bool partial=ResolveMatchingRulesFailOpen(rules,item,[&](const Rule& r){
        applied.push_back(r.marker);
        finalShow=r.show;
    });
    assert(partial);
    assert((applied==std::vector<int>{10}));
    assert(finalShow);

    // If a previously matched continued block was Hide, an unresolved later
    // block must still fail open for visibility at the plugin layer. The
    // generic resolver exposes the unknown callback for that policy.
    rules[0].show=false;
    applied.clear();
    finalShow=false;
    assert(ResolveMatchingRulesFailOpen(rules,item,[&](const Rule& r){
        applied.push_back(r.marker);
        finalShow=r.show;
    },[&]{ finalShow=true; }));
    assert((applied==std::vector<int>{10}));
    assert(finalShow);
    rules[0].show=true;

    // Six sockets matches the terminating Hide and stops before fallback.
    item.socketsKnown=true;
    item.sockets=6;
    applied.clear();
    assert(ResolveMatchingRulesFailOpen(rules,item,[&](const Rule& r){
        applied.push_back(r.marker);
        finalShow=r.show;
    }));
    assert((applied==std::vector<int>{10,20}));
    assert(!finalShow);

    // A known socket mismatch skips that block; the later fallback can match.
    item.sockets=5;
    applied.clear();
    assert(ResolveMatchingRulesFailOpen(rules,item,[&](const Rule& r){
        applied.push_back(r.marker);
        finalShow=r.show;
    }));
    assert((applied==std::vector<int>{10,30}));
    assert(finalShow);

    // Without Continue, the first matching block is terminal as before.
    rules[0].continueEvaluation=false;
    applied.clear();
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(ResolveMatchingRulesFailOpen(rules,item,[&](const Rule& r){
        applied.push_back(r.marker);
    }));
    assert((applied==std::vector<int>{10}));
}
