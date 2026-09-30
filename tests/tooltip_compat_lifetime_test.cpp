#include "src/tooltip_compat_lifetime.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

int main() {
    namespace L = SoE::LootFilter::TooltipCompatLifetime;

    auto state=std::make_shared<L::State>();
    std::atomic_bool entered{},release{},finished{};

    std::thread worker([&] {
        L::InvocationGuard guard(state);
        assert(guard.entered());
        entered.store(true,std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
        finished.store(true,std::memory_order_release);
    });

    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();

    L::BeginClose(state);

    {
        L::InvocationGuard refused(state);
        assert(!refused.entered());
    }

    std::atomic_bool waitReturned{};
    std::thread waiter([&] {
        L::WaitForQuiescence(state);
        waitReturned.store(true,std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!waitReturned.load(std::memory_order_acquire));

    release.store(true,std::memory_order_release);
    worker.join();
    waiter.join();

    assert(finished.load(std::memory_order_acquire));
    assert(waitReturned.load(std::memory_order_acquire));
    assert(state->inFlight.load(std::memory_order_acquire)==0);
    return 0;
}
