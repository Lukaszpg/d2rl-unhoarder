#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace SoE::LootFilter::TooltipCompatLifetime {

struct State final {
    std::atomic<std::uint32_t> inFlight{};
    std::atomic_bool closing{};
};

class InvocationGuard final {
public:
    explicit InvocationGuard(const std::shared_ptr<State>& state) noexcept
        : state_(state) {
        if (!state_ || state_->closing.load(std::memory_order_acquire)) {
            state_.reset();
            return;
        }
        state_->inFlight.fetch_add(1,std::memory_order_acq_rel);
        if (state_->closing.load(std::memory_order_acquire)) {
            Leave();
        }
    }

    InvocationGuard(const InvocationGuard&) = delete;
    InvocationGuard& operator=(const InvocationGuard&) = delete;

    ~InvocationGuard() noexcept { Leave(); }

    [[nodiscard]] bool entered() const noexcept { return state_ != nullptr; }

private:
    void Leave() noexcept {
        if (!state_) return;
        auto state=std::move(state_);
        if (state->inFlight.fetch_sub(1,std::memory_order_acq_rel)==1)
            state->inFlight.notify_all();
    }

    std::shared_ptr<State> state_{};
};

inline void BeginClose(const std::shared_ptr<State>& state) noexcept {
    if (state) state->closing.store(true,std::memory_order_release);
}

inline void WaitForQuiescence(const std::shared_ptr<State>& state) noexcept {
    if (!state) return;
    auto active=state->inFlight.load(std::memory_order_acquire);
    while (active!=0) {
        state->inFlight.wait(active,std::memory_order_acquire);
        active=state->inFlight.load(std::memory_order_acquire);
    }
}

} // namespace SoE::LootFilter::TooltipCompatLifetime
