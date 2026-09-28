#include "../src/native_pickup_guard_policy.hpp"
using namespace SoE::LootFilter::NativePickupGuardPolicy;
// User's 0.2.32 observation: D2R+0x101AE4 nearby and D2R+0xFA11A after
// movement must be decided solely by the action/type/id + fresh native checks.
static_assert(Candidate(22,4,126));
static_assert(Candidate(22,4,125));
static_assert(Candidate(22,4,129)); // hidden Battle Staff
static_assert(!Candidate(22,4,0));
static_assert(!Candidate(22,3,126));
static_assert(!Candidate(21,4,126));
static_assert(SameItemIdentity(4,126,126));
static_assert(!SameItemIdentity(4,125,126));
static_assert(!SameItemIdentity(0,126,126));
static_assert(GroundMode(3));
static_assert(!GroundMode(0));
static_assert(!GroundMode(4));
static_assert(!GroundMode(5));
int main(){}
