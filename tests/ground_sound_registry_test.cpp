#include "src/ground_sound_registry.hpp"
#include <cassert>
#include <cstdint>
using SoE::LootFilter::SoundIdentity::Registry;
int main() {
    Registry registry;
    using O=Registry::Observation;
    std::uint64_t first{}, next{}, duplicate{};
    assert(registry.Observe(155,0x6f766964,&first)==O::New); // divo
    assert(first != 0 && registry.IsCurrent(155,first));
    for(int i=0;i<250;i++) assert(registry.Observe(155,0x6f766964)==O::AlreadySeen);
    assert(registry.Observe(155,0x006f7865,&duplicate)==O::AlreadySeen);
    assert(duplicate==first); // code changes cannot replay an ID
    assert(registry.Observe(156,0x006f7865)==O::New); // exo
    assert(registry.Observe(0,0x6f766964)==O::Invalid);
    assert(registry.Observe(157,0)==O::Invalid);
    // Pickup of only 155 rearms 155, not the unrelated 156.
    assert(registry.Forget(155));
    assert(!registry.Forget(155));
    assert(!registry.IsCurrent(155,first));
    assert(registry.Observe(156,0x006f7865)==O::AlreadySeen);
    assert(registry.Observe(155,0x6f766964,&next)==O::New);
    assert(next!=first && registry.IsCurrent(155,next));
    registry.Clear(); // GameJoined / accepted JSON generation
    assert(!registry.IsCurrent(155,next));
    assert(registry.Observe(155,0x6f766964)==O::New);
    // Deletion must preserve collisions, and tombstones must be reusable.
    registry.Clear();
    constexpr std::uint32_t stride=static_cast<std::uint32_t>(Registry::SlotCount);
    for (std::uint32_t n=0;n<Registry::CollisionScanLimit;n++)
        assert(registry.Observe(1+n*stride,0x6f766964)==O::New);
    assert(registry.Observe(1+Registry::CollisionScanLimit*stride,0x6f766964)==O::Full);
    assert(registry.Forget(1));
    assert(registry.Observe(1+stride,0x6f766964)==O::AlreadySeen);
    assert(registry.Observe(1+Registry::CollisionScanLimit*stride,0x6f766964)==O::New);
    assert(!registry.Forget(2));
    assert(registry.Observe(1,0x6f766964)==O::Full);
}
