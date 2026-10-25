#include "src/native_drop_sound.hpp"
#include <cassert>
#include <vector>
#include <cstdio>
int main() {
    using namespace UnHoarder::NativeDropSound;
    assert(IsBundledFilterSound("Filter01") && IsBundledFilterSound("Filter16"));
    for(const auto* name:{"Filter00","Filter17","Filter99","Filter1X","Other001","Filter001"})
        assert(!IsBundledFilterSound(name));
    Snapshot item{4,520,137,5};int nativeCalls{},customCalls{},reads{},unit{};
    bool selected=true,accepted=true,readable=true;
    auto original=[&](void* p){assert(p==&unit);++nativeCalls;};
    auto read=[&](void* p,Snapshot& out){assert(p==&unit);++reads;out=item;return readable;};
    auto replace=[&](void* p,const Snapshot& out){assert(p==&unit && out.id==137);++customCalls;return selected && accepted;};
    for(auto caller:{0x1c36a0u,0x1c6c9eu})for(auto mode:{3u,5u}) {
        nativeCalls=customCalls=0;item.mode=mode;
        Dispatch(caller,&unit,original,read,replace);
        assert(customCalls==1 && nativeCalls==0);
    }
    // Same ID can drop again immediately, without a label render or inventory poll.
    Dispatch(0x1c36a0,&unit,original,read,replace);
    assert(customCalls==2 && nativeCalls==0);
    for(bool missingSelection:{false,true}) {
        nativeCalls=customCalls=0;selected=!missingSelection;accepted=false;
        Dispatch(0x1c36a0,&unit,original,read,replace);
        assert(customCalls==1 && nativeCalls==1);
    }
    selected=accepted=true;
    for(auto caller:{0u,0x1517af6u,0x1519e46u,0x1c36a1u,0x1c6c9du}) {
        nativeCalls=customCalls=0;Dispatch(caller,&unit,original,read,replace);
        assert(nativeCalls==1 && customCalls==0);
    }
    for(auto mode:{0u,1u,2u,4u,6u,UINT32_MAX}) {
        item.mode=mode;nativeCalls=customCalls=0;
        Dispatch(0x1c36a0,&unit,original,read,replace);
        assert(nativeCalls==1 && customCalls==0);
    }
    item.mode=5;
    for(auto id:{0u,UINT32_MAX}) {item.id=id;nativeCalls=customCalls=0;
        Dispatch(0x1c36a0,&unit,original,read,replace);assert(nativeCalls==1 && customCalls==0);}
    item.id=137;readable=false;nativeCalls=customCalls=0;
    Dispatch(0x1c36a0,&unit,original,read,replace);assert(nativeCalls==1 && customCalls==0);
    readable=true;nativeCalls=customCalls=0;reads=0;
    Dispatch(0x1c36a0,&unit,original,[&](void*,Snapshot& out){out=item;if(++reads==2)++out.id;return true;},replace);
    assert(nativeCalls==1 && customCalls==0);
    std::vector<unsigned char> image(0x1d0000);
    struct Witness{std::uintptr_t at;const unsigned char* data;std::size_t size;};
    const Witness witnesses[]={{0x1c8130,Witness0,sizeof(Witness0)},{0x1c3659,Witness1,sizeof(Witness1)},
        {0x1c6c56,Witness2,sizeof(Witness2)},{0x1c37d4,Witness3,sizeof(Witness3)},{0x1a0c00,Witness4,sizeof(Witness4)}};
    for(const auto& w:witnesses)std::memcpy(image.data()+w.at,w.data,w.size);
    auto memory=[&](std::uintptr_t at,void* out,std::size_t size){if(at>image.size() || size>image.size()-at)return false;std::memcpy(out,image.data()+at,size);return true;};
    assert(Verify(memory));
    for(const auto& w:witnesses)for(std::size_t i=0;i<w.size;++i){image[w.at+i]^=1;assert(!Verify(memory));image[w.at+i]^=1;}
    assert(!Verify([](std::uintptr_t,void*,std::size_t){return false;}));
    std::puts("Native drop replacement, no-selection/playback-failure fallback, repeated drops, label-caller rejection, stale identities and every witness-byte mutation pass.");
}
