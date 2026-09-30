#include "../src/native_pickup_guard_policy.hpp"
#include <string_view>
using namespace UnHoarder::NativePickupGuardPolicy;
static_assert(DecisionName(Decision::Blocked)==std::string_view{"hidden-ground-rule"});
static_assert(DecisionName(Decision::GuardInactive)==std::string_view{"guard-inactive"});
static_assert(DecisionName(Decision::NotGround)==std::string_view{"not-ground"});
static_assert(DecisionName(Decision::LookupFailed)==std::string_view{"lookup-failed"});
static_assert(DecisionName(Decision::NoHiddenRule)==std::string_view{"visible-or-no-hidden-rule"});
static_assert(DecisionName(Decision::InvalidCode)==std::string_view{"invalid-item-code"});
static_assert(DecisionName(Decision::RulesModeInactive)==std::string_view{"rules-mode-inactive"});
static_assert(DecisionName(Decision::NoHiddenRules)==std::string_view{"no-active-hidden-rules"});
int main() {}
