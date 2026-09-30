#include "../src/filter_rule_engine.hpp"
#include <cassert>
#include <vector>
using namespace UnHoarder::RuleEngine;
struct Rule {bool usesConditions{true}; std::uint32_t code{}; Conditions conditions{}; bool show{true};};
int main() {
  Rule codeOnly{};codeOnly.conditions.codes={1};
  Rule socket{};socket.conditions.baseCodes={2};socket.conditions.sockets.enabled=true;
  socket.conditions.sockets.hasMin=true;socket.conditions.sockets.min=4;
  Rule hide{};hide.show=false;
  std::vector<Rule> rules{codeOnly,socket,hide};
  Item item{};item.code=1;
  assert(NextNativeProperty(rules,item)==NextProperty::None);
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[0]);
  item.code=2;
  assert(NextNativeProperty(rules,item)==NextProperty::Sockets);
  assert(!FirstMatchFailOpenProperties(rules,item)); // unavailable is NOT zero
  item.socketsKnown=true;item.sockets=4;
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
  assert(NextNativeProperty(rules,item)==NextProperty::None);
  item.sockets=3;
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
  socket.conditions.sockets.hasMin=false;
  socket.conditions.sockets.hasEq=true;socket.conditions.sockets.eq=0;
  rules={codeOnly,socket,hide};
  item.sockets=0;assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
  item.socketsKnown=false;
  assert(!FirstMatchFailOpenProperties(rules,item));
  Rule rareSocket=socket;rareSocket.conditions.sockets.hasEq=false;
  rareSocket.conditions.sockets.hasMin=true;rareSocket.conditions.sockets.min=4;
  rareSocket.conditions.qualities={7};
  rules={codeOnly,rareSocket,hide};
  item.qualityKnown=false;item.socketsKnown=false;
  assert(NextNativeProperty(rules,item)==NextProperty::QualityLevel);
  item.qualityKnown=true;item.quality=7;
  assert(NextNativeProperty(rules,item)==NextProperty::Sockets);
  item.socketsKnown=true;item.sockets=4;
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[1]);
  item.quality=2;item.socketsKnown=false;
  assert(NextNativeProperty(rules,item)==NextProperty::None);
  assert(FirstMatchFailOpenProperties(rules,item)==&rules[2]);
  // A socket-only first rule must not read quality for a later rarity rule.
  Rule laterRare{};laterRare.conditions.baseCodes={2};laterRare.conditions.qualities={7};
  rules={socket,laterRare,hide};item.qualityKnown=false;item.socketsKnown=false;
  assert(NextNativeProperty(rules,item)==NextProperty::Sockets);
  item.socketsKnown=true;item.sockets=0;
  assert(NextNativeProperty(rules,item)==NextProperty::None);
  item.sockets=1;
  assert(NextNativeProperty(rules,item)==NextProperty::QualityLevel);
  // A known-nonmatching code skips properties of its rule.
  item.code=99;item.qualityKnown=false;item.socketsKnown=false;
  assert(NextNativeProperty(rules,item)==NextProperty::None);
}
