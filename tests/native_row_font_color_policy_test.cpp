#include "src/native_row_font_color_policy.hpp"
#include <array>
#include <cassert>
#include <limits>
int main() {
    using namespace NativeRowFontColorPolicy;
    assert(VanillaGroundLabel({0.941f,0.941f,0.941f,1.f}));
    assert(VanillaGroundLabel({0.92f,0.94f,0.97f,0.995f}));
    assert(!VanillaGroundLabel({1.f,1.f,1.f,1.f}));
    assert(!VanillaGroundLabel({0.941f,0.941f,0.941f,0.5f}));
    assert(!VanillaGroundLabel({0.2f,0.2f,0.2f,1.f}));
    assert(!VanillaGroundLabel({std::numeric_limits<float>::quiet_NaN(),0.941f,0.941f,1.f}));
    assert(ValidJsonColor({1.f,50.f/255.f,50.f/255.f,1.f}));
    assert(!ValidJsonColor({2.f,0.f,0.f,1.f}));
    assert(!ValidJsonColor({-0.1f,0.f,0.f,1.f}));
    assert(!ValidJsonColor({0.f,0.f,0.f,std::numeric_limits<float>::infinity()}));
}
