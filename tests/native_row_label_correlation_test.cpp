#include "src/native_row_label_correlation.hpp"
#include <cassert>
#include <string_view>
#include <iostream>
int main() {
    using namespace NativeRowLabelCorrelation;
    const std::string_view native="\xEE\x81\xBE\x3D" "Divine Orb";
    const std::string_view otherPalette="\xEE\x81\xBE\x30" "Divine Orb";
    assert(Compare(native,native)==Relation::Exact);
    assert(Compare(native,otherPalette)==Relation::ColorPrefixOnly);
    assert(Compare(native,"Divine Orb")==Relation::ColorPrefixOnly);
    assert(Compare(native,"Jah Rune")==Relation::Different);
    assert(Compare(native,"")==Relation::Unavailable);
    assert(Compare("","Divine Orb")==Relation::Unavailable);
    // Same text from two different item units is still indistinguishable
    // by label alone. This helper deliberately never returns a UnitAny ID.
    assert(Compare(native,native)==Relation::Exact);
    std::cout<<"0.1.95 native row label comparison (no ownership claim): PASS\n";
}
