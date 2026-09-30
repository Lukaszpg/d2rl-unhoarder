#include "../src/filter_rule_engine.hpp"
#include <cassert>
#include <vector>
using namespace UnHoarder::RuleEngine;
struct Rule {bool usesConditions{true};std::uint32_t code{};Conditions conditions{};bool show{true};};
int main() {
  Rule divo{};divo.conditions.codes={123};divo.show=true;
  Rule exo{};exo.conditions.codes={124,125};exo.show=true;
  Rule unique{};unique.conditions.codes={126};unique.conditions.qualities={7};
  unique.show=false;
  Rule broadHide{};broadHide.show=false;
  std::vector<Rule> rules{divo,exo,unique,broadHide};
  Item item{};item.code=123;item.classIdKnown=true;item.classId=56;
  assert(!NeedsNativeQualityLevel(rules,item));
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[0]);
  item.code=124;assert(!NeedsNativeQualityLevel(rules,item));
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
  item.code=126;assert(NeedsNativeQualityLevel(rules,item));
  assert(!FirstMatchFailOpenProperties(rules,item)); // unknown: fail OPEN
  item.qualityKnown=true;item.quality=7;
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
  item.quality=2;
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[3]);
  // A broad property rule before a code-specific rule does require the reader.
  Rule broadRare{};broadRare.conditions.qualities={6};broadRare.show=false;
  rules={broadRare,divo};item.code=123;item.qualityKnown=false;
  assert(NeedsNativeQualityLevel(rules,item));
  assert(!FirstMatchFailOpenProperties(rules,item));
  item.qualityKnown=true;item.quality=2;
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
  // A code-specific property rule that cannot match the code is skipped.
  Rule rareOther{};rareOther.conditions.baseCodes={999};
  rareOther.conditions.qualities={6};rules={rareOther,divo};
  item.qualityKnown=false;
  assert(!NeedsNativeQualityLevel(rules,item));
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
  // A baseName resolved to uar must not force a native read for divo.
  Rule rareSacred{};rareSacred.conditions.baseCodes={126};
  rareSacred.conditions.qualities={7};rules={divo,rareSacred,broadHide};
  item.code=123;item.qualityKnown=false;
  assert(!NeedsNativeQualityLevel(rules,item));
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[0]);
  item.code=126;
  assert(NeedsNativeQualityLevel(rules,item));
  assert(!FirstMatchFailOpenProperties(rules,item));
  // A quality-failing Show must block a later hide until the quality is known.
  Rule showRare{};showRare.conditions.codes={123};
  showRare.conditions.qualities={6};showRare.show=true;
  rules={showRare,broadHide};item.code=123;
  assert(NeedsNativeQualityLevel(rules,item));
  assert(!FirstMatchFailOpenProperties(rules,item));
  item.qualityKnown=true;item.quality=6;
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[0]);
  item.quality=2;
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
  Rule highLvl{};highLvl.conditions.codes={123};highLvl.conditions.itemLevel.enabled=true;
  highLvl.conditions.itemLevel.hasMin=true;highLvl.conditions.itemLevel.min=85;
  rules={highLvl,broadHide};item.qualityKnown=false;item.itemLevelKnown=false;
  assert(NeedsNativeQualityLevel(rules,item));
  assert(!FirstMatchFailOpenProperties(rules,item));
  item.itemLevelKnown=true;item.itemLevel=88;
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[0]);
}
