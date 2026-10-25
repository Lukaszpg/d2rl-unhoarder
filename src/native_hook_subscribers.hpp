#pragma once
#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>

// Static, bounded registration. Never hold the mutex while invoking a foreign
// callback. Unregister is a quiescence barrier, and must not be called by a
// subscriber from inside its own callback. No plugin-owned pointer is retained
// after successful unregister. No allocation or lock in the callback body.
namespace UnHoarder::NativeHookSubscribers {
template<class Callback,std::size_t Capacity=8>
class Registry {
    struct Slot {const void* owner{};Callback callback{};void* user{};unsigned inFlight{};};
    struct Pending {std::size_t index{};Callback callback{};void* user{};};
    std::mutex mutex_;
    std::condition_variable idle_;
    std::array<Slot,Capacity> slots_{};
    inline static thread_local bool executing_=false;
public:
    bool Register(const void* owner,Callback callback,void* user) noexcept {
        if(!owner || !callback)return false;
        std::lock_guard lock(mutex_);
        for(const auto& slot:slots_)
            if(slot.owner==owner && slot.callback==callback && slot.user==user)return false;
        for(auto& slot:slots_)if(!slot.owner && !slot.inFlight) {
            slot={owner,callback,user,0};return true;
        }
        return false;
    }
    bool Unregister(const void* owner,Callback callback,void* user) noexcept {
        if(!owner || !callback || executing_)return false;
        std::unique_lock lock(mutex_);
        for(auto& slot:slots_)if(slot.owner==owner && slot.callback==callback && slot.user==user) {
            slot.callback=nullptr; // no new dispatch may capture it
            idle_.wait(lock,[&slot]{return slot.inFlight==0;});
            slot={};return true;
        }
        return false;
    }
    template<class Invoke> bool Dispatch(Invoke invoke) noexcept {
        if(executing_)return false;
        std::array<Pending,Capacity> pending{};
        std::size_t count=0;
        {
            std::lock_guard lock(mutex_);
            for(std::size_t i=0;i<Capacity;++i) {
                auto& slot=slots_[i];
                if(!slot.owner || !slot.callback)continue;
                ++slot.inFlight;
                pending[count++]={i,slot.callback,slot.user};
            }
        }
        executing_=true;
        bool handled=false;
        for(std::size_t i=0;i<count;++i) {
            if(!handled)handled=invoke(pending[i].callback,pending[i].user);
            {
                std::lock_guard lock(mutex_);
                auto& slot=slots_[pending[i].index];
                if(--slot.inFlight==0)idle_.notify_all();
            }
        }
        executing_=false;
        return handled;
    }
};
}
