#include "../src/automap_projection.hpp"

#include <cassert>
#include <cstdint>

using namespace SoE::LootFilter::AutomapProjection;

int main() {
    Point client{};
    assert(WorldSubtileToClient(5110, 5015, client));
    assert(client.x == 1520);
    assert(client.y == 81000);
    assert(PackClientCoordinates(client)
        == (static_cast<std::uint64_t>(static_cast<std::uint32_t>(81000)) << 32U)
            + 1520U);

    assert(WorldSubtileToClient(5072, 5154, client));
    assert(client.x == -1312);
    assert(client.y == 81808);

    const ClipRect clip{100, 50, 800, 600};
    assert(PlausibleClip(clip));
    assert(Contains(clip, Point{100, 50}));
    assert(Contains(clip, Point{899, 649}));
    assert(!Contains(clip, Point{900, 649}));
    assert(!Contains(clip, Point{899, 650}));
    assert(!PlausibleClip(ClipRect{0, 0, 0, 600}));
    assert(!PlausibleClip(ClipRect{0, 0, 40000, 600}));
    return 0;
}
