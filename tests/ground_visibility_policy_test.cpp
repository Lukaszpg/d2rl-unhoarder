#include "src/ground_visibility_policy.hpp"
#include "src/ground_quantity_label.hpp"
#include <cassert>
using SoE::LootFilter::GroundVisibility::ConcealBulkVisuals;
using SoE::LootFilter::GroundQuantity::MatchesRuleName;
int main() {
    // A native bulk item-label paint is concealed regardless of how labels
    // were activated: hold, toggle or an alternative input binding.
    assert(ConcealBulkVisuals(true,true,true,true,true,true,true));
    assert(!ConcealBulkVisuals(false,true,true,true,true,true,true));
    assert(!ConcealBulkVisuals(true,false,true,true,true,true,true));
    assert(!ConcealBulkVisuals(true,true,false,true,true,true,true));
    assert(!ConcealBulkVisuals(true,true,true,false,true,true,true));
    assert(!ConcealBulkVisuals(true,true,true,true,false,true,true));
    assert(!ConcealBulkVisuals(true,true,true,true,true,false,true));
    assert(!ConcealBulkVisuals(true,true,true,true,true,true,false));
    assert(MatchesRuleName("12x Exalted Orb","Exalted Orb",12));
    assert(!MatchesRuleName("12x Exalted Orb","Exalted Orb",4));
}
