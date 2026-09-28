#include "../src/minimap_world_position.hpp"
#include <array>
#include <cassert>
using namespace SoE::LootFilter::MinimapWorldPosition;
int main() {
    std::array<std::uint8_t,0x28> path{};
    path[0x10]=0x34;path[0x11]=0x12;
    path[0x14]=0x78;path[0x15]=0x56;
    auto result=Decode(path.data(),path.size());
    assert(result.available && result.plausible);
    assert(result.x==0x1234 && result.y==0x5678);
    assert(!Decode(nullptr,0x28).available);
    assert(!Decode(path.data(),0x17).available);
    path[0x10]=0;path[0x11]=0;
    assert(!Decode(path.data(),path.size()).plausible);
    path[0x10]=1;path[0x11]=0;
    path[0x17]=1;
    assert(!Decode(path.data(),path.size()).plausible);
}
