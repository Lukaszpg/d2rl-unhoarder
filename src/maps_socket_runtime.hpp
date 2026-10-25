#pragma once
#include "maps_socket_memory.hpp"
#include "maps_socket_core_profile.hpp"
#include "maps_socket_hash.hpp"
#include <mutex>

namespace UnHoarder::MapsSocket {
inline constexpr unsigned char MapsHash[]={0xd9,0x63,0xca,0x4a,0xaa,0xfd,0x70,0xd2,0xed,0x55,0x86,0x4d,0xce,0xd9,0x56,0x59,0x52,0x90,0xb5,0x23,0xf8,0x92,0x2c,0x7d,0x43,0xbc,0x09,0x39,0xfe,0xb4,0xe1,0xa1};
inline constexpr unsigned char CoreHash[]={0x2a,0x86,0x8d,0x01,0x3d,0x2e,0x08,0x30,0xbd,0x2d,0x9e,0x04,0xb9,0x18,0xb1,0x9e,0x46,0xa7,0x3c,0xf7,0x26,0xc8,0x33,0xe7,0x0d,0x08,0x9b,0x94,0x8f,0xde,0xb5,0xa2};
struct State {HMODULE maps{},core{};bool checked{},admitted{},stopping{};};
inline State state{};
inline std::mutex mutex;

template<size_t N> bool Match(const ImageMemory& image,uintptr_t at,const unsigned char (&expected)[N]) noexcept {
    unsigned char bytes[N]{};
    return image.Read(at,bytes,N,Kind::Code) && std::memcmp(bytes,expected,N)==0;
}
inline bool Guard(uintptr_t game) noexcept {
    if(!game || game>UINTPTR_MAX-0x3e2a220 || !state.maps || !state.core)return false;
    unsigned char bridge[sizeof(CoreProfile::Entry)]{};uintptr_t owner{};
    if(!ImageMemory::Copy(game+0x2f5020,bridge,sizeof(bridge)) || std::memcmp(bridge,CoreProfile::Entry,sizeof(bridge)) ||
        !ImageMemory::Copy(game+0x3e2a218,&owner,8) || owner!=reinterpret_cast<uintptr_t>(state.maps)+0x132f0)return false;
    ImageMemory maps{},core{};
    return maps.Open(state.maps) && core.Open(state.core) &&
        Match(core,core.base+0x831de0,CoreProfile::Wrapper) &&
        Match(core,core.base+0x3d8bd0,CoreProfile::Body) &&
        Validate(maps.Reader(),owner,core.base+0x831de0);
}
inline bool Initialize(uintptr_t game) noexcept {
    if(state.checked)return state.admitted;
    const auto maps=GetModuleHandleW(L"d2rl-maps.dll"),core=GetModuleHandleW(L"D2RCore.dll");
    if(!maps || !core)return false;
    // Pin the reviewed images while caching disk identity. Every reachable
    // code block, forwarding slot, bridge and ownership/page gate is rechecked
    // on BOTH sides of every socket read. No arbitrary foreign owner is admitted.
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(maps),&state.maps))return false;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(core),&state.core)) {
        FreeLibrary(state.maps);state.maps=nullptr;return false;
    }
    state.checked=true;
    state.admitted=VerifyFileHash(state.maps,MapsHash) && VerifyFileHash(state.core,CoreHash) && Guard(game);
    return state.admitted;
}
inline bool Read(uintptr_t game,void* item,int64_t& out) noexcept {
    const std::unique_lock lock(mutex,std::try_to_lock);
    if(!lock.owns_lock() || state.stopping || !item || !Initialize(game) || !Guard(game))return false;
    uint32_t before[4]{},after[4]{};
    if(!ImageMemory::Copy(reinterpret_cast<uintptr_t>(item),before,sizeof(before)) || before[0]!=4 || !before[2] ||
        before[2]==UINT32_MAX || (before[3]!=3 && before[3]!=5))return false;
    using Reader=int64_t(__fastcall*)(void*,int32_t,uint16_t);
    // Keep the installed game bridge and Maps hook in the call chain.
    // This function exposes NO arbitrary-stat capability and no guessed zero.
    const auto value=reinterpret_cast<Reader>(game+0x2f5020)(item,194,0);
    if(!Guard(game) || !ImageMemory::Copy(reinterpret_cast<uintptr_t>(item),after,sizeof(after)) ||
        std::memcmp(before,after,sizeof(before)) || value<0 || value>15)return false;
    out=value;return true;
}
inline void Shutdown() noexcept {
    const std::lock_guard lock(mutex);state.stopping=true;state.admitted=false;
    if(state.core)FreeLibrary(state.core);
    if(state.maps)FreeLibrary(state.maps);
    state.core=state.maps=nullptr;
}
}
