#include "src/native_hook_subscribers.hpp"
#include <cassert>
#include <cstdio>
namespace {
using Drop=bool(*)(void*,void*) noexcept;
bool Replacer(void* p,void* user) noexcept {
    auto* hits=static_cast<int*>(user);
    assert(p==hits);
    ++*hits;
    return true;
}
bool Fallback(void*,void* user) noexcept {
    ++*static_cast<int*>(user);
    return false;
}
}
int main() {
    UnHoarder::NativeHookSubscribers::Registry<Drop> listeners;
    int hits=0,other=0,owner1=1,owner2=2;
    assert(listeners.Register(&owner1,&Fallback,&other));
    assert(listeners.Register(&owner2,&Replacer,&hits));
    assert(!listeners.Register(&owner1,&Fallback,&other));
    assert(listeners.Dispatch([&](Drop cb,void* data) noexcept {
        return cb(&hits,data);
    }));
    assert(hits==1 && other==1);
    assert(!listeners.Unregister(&owner2,&Replacer,&other));
    assert(listeners.Unregister(&owner2,&Replacer,&hits));
    assert(!listeners.Dispatch([&](Drop cb,void* data) noexcept {
        return cb(&hits,data);
    }));
    assert(other==2);
    assert(listeners.Unregister(&owner1,&Fallback,&other));
    assert(!listeners.Unregister(&owner1,&Fallback,&other));
    std::puts("Cooperative native hook subscriber registration and forwarding pass.");
}
