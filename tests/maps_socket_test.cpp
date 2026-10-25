#include "src/maps_socket_contract.hpp"
#include <windows.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
using namespace UnHoarder::MapsSocket;
struct Node{uintptr_t at;Kind kind;std::vector<unsigned char> bytes;};
struct Fixture {
    uintptr_t hook=0x1800132f0,original=0x190831de0,slot=0x1800feb70,levelSlot=0x1800feb80,helper=0x1800142d0,tail=0x1800135a0;
    std::vector<Node> nodes;
    void RefAt(std::vector<unsigned char>& b,size_t at,size_t end,uintptr_t from,uintptr_t to){const auto delta=static_cast<int32_t>(to-(from+end));std::memcpy(b.data()+at,&delta,4);}
    Fixture(){
        nodes={{hook,Kind::Code,{Profile::Head,Profile::Head+sizeof(Profile::Head)}},
            {hook+0x83,Kind::Code,{Profile::Gate,Profile::Gate+sizeof(Profile::Gate)}},
            {helper,Kind::Code,{Profile::Index,Profile::Index+sizeof(Profile::Index)}},
            {tail,Kind::Code,{Profile::Return,Profile::Return+sizeof(Profile::Return)}},
            {helper+0x72,Kind::Code,{Profile::Negative,Profile::Negative+sizeof(Profile::Negative)}},
            {slot,Kind::Data,std::vector<unsigned char>(8)},
            {levelSlot,Kind::Data,std::vector<unsigned char>(8)}};
        RefAt(nodes[0].bytes,0x19,0x1d,hook,slot);
        RefAt(nodes[1].bytes,0x1a,0x1f,hook+0x83,levelSlot);
        RefAt(nodes[1].bytes,0x27,0x2b,hook+0x83,tail);
        RefAt(nodes[1].bytes,0x48,0x4c,hook+0x83,tail);
        RefAt(nodes[1].bytes,0x33,0x37,hook+0x83,helper);
        std::memcpy(nodes[5].bytes.data(),&original,8);
    }
    Memory Reader(){return {this,[](void* p,uintptr_t at,void* out,size_t size,Kind kind) noexcept {
        for(const auto& n:static_cast<Fixture*>(p)->nodes)if(n.at==at && n.bytes.size()==size && n.kind==kind){std::memcpy(out,n.bytes.data(),size);return true;}return false;
    }};}
    bool Valid(){return Validate(Reader(),hook,original);}
};
unsigned checks{},calls{};void* seenUnit{};int seenStat{};uint16_t seenLayer{};int64_t result{};
void Check(bool ok,const char* message){++checks;if(!ok){std::printf("FAIL %s\n",message);std::exit(1);}}
int64_t __fastcall Original(void* unit,int32_t stat,uint16_t layer){++calls;seenUnit=unit;seenStat=stat;seenLayer=layer;return result;}
void ExecuteNativePath(){
    // Execute the actual captured Maps instructions in this PRIVATE process.
    // The non-taken stat-127/player-modifier branches are deliberately INT3.
    auto* storage=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x20000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));Check(storage!=nullptr,"allocate fixture");
    const auto base=reinterpret_cast<uintptr_t>(storage);Fixture f;
    const auto oldHook=f.hook;f.hook=base+0x1000;f.helper=base+0x3000;f.tail=base+0x4000;f.slot=base+0x10000;f.levelSlot=base+0x10010;f.original=reinterpret_cast<uintptr_t>(&Original);
    f.nodes[0].at=f.hook;f.nodes[1].at=f.hook+0x83;f.nodes[2].at=f.helper;f.nodes[3].at=f.tail;f.nodes[4].at=f.helper+0x72;f.nodes[5].at=f.slot;f.nodes[6].at=f.levelSlot;
    f.RefAt(f.nodes[0].bytes,0x19,0x1d,f.hook,f.slot);
    f.RefAt(f.nodes[1].bytes,0x1a,0x1f,f.hook+0x83,f.levelSlot);
    f.RefAt(f.nodes[1].bytes,0x27,0x2b,f.hook+0x83,f.tail);
    f.RefAt(f.nodes[1].bytes,0x48,0x4c,f.hook+0x83,f.tail);
    f.RefAt(f.nodes[1].bytes,0x33,0x37,f.hook+0x83,f.helper);
    std::memcpy(f.nodes[5].bytes.data(),&f.original,8);
    std::memset(storage,0xcc,0x10000);
    for(const auto& n:f.nodes)std::memcpy(reinterpret_cast<void*>(n.at),n.bytes.data(),n.bytes.size());
    Check(f.Valid(),"native graph validates");DWORD old{};Check(VirtualProtect(storage,0x10000,PAGE_EXECUTE_READ,&old)!=0,"fixture RX");
    Check(FlushInstructionCache(GetCurrentProcess(),storage,0x10000)!=0,"flush fixture");
    using Read=int64_t(__fastcall*)(void*,int32_t,uint16_t);auto read=reinterpret_cast<Read>(f.hook);uint32_t unit[4]{4,22,137,3};
    for(uintptr_t level:{uintptr_t{0},uintptr_t{0x12345678}}){std::memcpy(storage+0x10010,&level,8);
        for(int64_t value:{int64_t{0},int64_t{1},int64_t{6},int64_t{15},int64_t{-1},int64_t{0x100000000}}){
            result=value;calls=0;Check(read(unit,194,0)==value,"socket path preserves full return value");
            Check(calls==1 && seenUnit==unit && seenStat==194 && seenLayer==0,"original called exactly once with unchanged socket arguments");
        }
    }
    VirtualFree(storage,0,MEM_RELEASE);(void)oldHook;
}
int main(){
    Fixture good;Check(good.Valid(),"known complete graph admitted");
    for(size_t n=0;n<good.nodes.size();++n){
        for(size_t i=0;i<good.nodes[n].bytes.size();++i){
            // The borrowed level callback value is never invoked for stat194;
            // both null and non-null values are intentionally admitted.
            if(n==6)continue;
            // Unused Index jump-table operands are not executed by stat194.
            if(n==2 && ((i>=0x25 && i<0x29)||(i>=0x2d && i<0x31)||(i>=0x34 && i<0x38)))continue;
            Fixture bad;bad.nodes[n].bytes[i]^=1;Check(!bad.Valid(),"changed reachable graph refused");
        }
        Fixture bad;bad.nodes[n].kind=static_cast<Kind>(99);Check(!bad.Valid(),"wrong code/data ownership refused");
        Fixture missing;missing.nodes.erase(missing.nodes.begin()+n);Check(!missing.Valid(),"missing graph node refused");
    }
    Check(!Validate({},good.hook,good.original),"unreadable memory refused");
    ExecuteNativePath();std::printf("%u socket forwarding graph and native execution checks passed.\n",checks);
}
