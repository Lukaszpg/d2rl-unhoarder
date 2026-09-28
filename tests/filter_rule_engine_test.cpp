#include "../src/filter_rule_engine.hpp"
#include <cassert>
#include <vector>
using namespace SoE::LootFilter;
struct TestRule { bool schema2{}; std::uint32_t code{}; RuleEngine::Conditions conditions{}; bool show{true}; int marker{}; };
int main() {
    auto r=[](int m,bool show) { TestRule rule{}; rule.schema2=true;rule.marker=m;rule.show=show;return rule; };
    // Excel `name` is resolved to base code at reload; the runtime item
    // class ID must not enter the match, even if unknown or different.
    auto specific=r(1,true); specific.conditions.codes={123}; specific.conditions.baseCodes={123,124};
    auto fallback=r(2,false); fallback.conditions.codes={123};
    std::vector<TestRule> rules{specific,fallback};
    RuleEngine::Item item{};item.code=123;item.classIdKnown=true;item.classId=50;
    assert(RuleEngine::FirstMatch(rules,item)->marker==1);
    item.classId=60; assert(RuleEngine::FirstMatch(rules,item)->marker==1);
    item.classIdKnown=false; assert(RuleEngine::FirstMatch(rules,item)->marker==1);
    item.code=999; assert(!RuleEngine::FirstMatch(rules,item));
    TestRule q=r(3,false);q.conditions.codes={123};q.conditions.quantity.enabled=true;
    q.conditions.quantity.hasMin=true;q.conditions.quantity.min=3;
    rules={q};item.code=123;item.quantityKnown=false;
    assert(!RuleEngine::FirstMatch(rules,item));
    item.quantityKnown=true;item.quantity=2;assert(!RuleEngine::FirstMatch(rules,item));
    item.quantity=3;assert(RuleEngine::FirstMatch(rules,item));
    q.conditions.quantity.minInclusive=false;rules={q};
    assert(!RuleEngine::FirstMatch(rules,item));
    item.quantity=4;assert(RuleEngine::FirstMatch(rules,item));
    // Multiple codes/baseNames are OR inside their field; different fields
    // are AND. A matched first Show must stop before a later Hide.
    auto show=r(10,true);show.conditions.codes={123,125};
    show.conditions.baseCodes={123,124};
    auto hide=r(11,false);hide.conditions.codes={123};
    rules={show,hide};item.code=123;item.classId=60;
    assert(RuleEngine::FirstMatch(rules,item)->marker==10);
    item.code=125;assert(!RuleEngine::FirstMatch(rules,item));
    item.code=124;assert(!RuleEngine::FirstMatch(rules,item));
    item.code=126;assert(!RuleEngine::FirstMatch(rules,item));
    auto bounds=r(12,false);bounds.conditions.quantity.enabled=true;
    bounds.conditions.quantity.hasEq=true;bounds.conditions.quantity.eq=4;
    bounds.conditions.quantity.hasMin=true;bounds.conditions.quantity.min=3;
    bounds.conditions.quantity.hasMax=true;bounds.conditions.quantity.max=5;
    bounds.conditions.quantity.maxInclusive=false;
    rules={bounds};item.quantityKnown=true;item.quantity=4;
    assert(RuleEngine::FirstMatch(rules,item));
    item.quantity=5;assert(!RuleEngine::FirstMatch(rules,item));
    item.quantityKnown=false;assert(!RuleEngine::FirstMatch(rules,item));
    TestRule legacy{};legacy.code=123;legacy.marker=4;legacy.show=false;
    rules={legacy};item.code=123;assert(RuleEngine::FirstMatch(rules,item)->marker==4);
    item.code=124;assert(!RuleEngine::FirstMatch(rules,item));
}
