#include "minimap_icon_policy.hpp"
#include <cassert>

using namespace SoE::LootFilter::MinimapIconPolicy;

int main() {
    static_assert(MinimumSizePx == 12);
    static_assert(DefaultSizePx == 12.0F);
    static_assert(MaximumSizePx == 40);
    float value{};
    assert(TryNormalizeSizePx(-100, value) && value == 12.0F);
    assert(TryNormalizeSizePx(0, value) && value == 12.0F);
    assert(TryNormalizeSizePx(1, value) && value == 12.0F);
    assert(TryNormalizeSizePx(11, value) && value == 12.0F);
    assert(TryNormalizeSizePx(12, value) && value == 12.0F);
    assert(TryNormalizeSizePx(24, value) && value == 24.0F);
    assert(TryNormalizeSizePx(40, value) && value == 40.0F);
    assert(TryNormalizeSizePx(41, value) && value == 40.0F);
    assert(TryNormalizeSizePx(400, value) && value == 40.0F);
}
