#include "../src/filter_rule_engine.hpp"
#include "../src/base_name_table.hpp"
#include <cassert>
#include <vector>
using namespace SoE::LootFilter;
using namespace SoE::LootFilter::RuleEngine;
struct Rule {bool schema2{true};std::uint32_t code{};Conditions conditions{};bool show{true};};
int main() {
    const auto code=[](std::string_view c){return BaseNameTable::PackCode(c);};
    Rule codeOnly{};codeOnly.conditions.codes={code("divo")};
    Rule swords{};swords.conditions.typeCodes={code("7cr"),code("7ls")};
    swords.conditions.qualities={7};
    Rule fallback{};fallback.show=false;
    std::vector<Rule> rules{codeOnly,swords,fallback};
    Item item{};item.code=code("divo");
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[0]);
    item.code=code("7wa");
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
    item.code=code("7cr");
    assert(NextNativeProperty(rules,item)==NextProperty::QualityLevel);
    assert(!FirstMatchFailOpenProperties(rules,item));
    item.qualityKnown=true;item.quality=7;
    assert(NextNativeProperty(rules,item)==NextProperty::None);
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
    item.code=code("7wa");
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
    item.code=code("7cr");item.quality=6;
    assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
}
