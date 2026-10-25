#pragma once
#include <D2RLPlugin/context.h>
#include <cstdint>
#include <type_traits>

// Opt-in, owner-mediated cooperation for the exact native drop-audio and
// automap entry points. Consumers must resolve the entry's actual tracked
// D2RLoader hook owner before acquiring this service from that provider.
// Callbacks borrow game-owned pointers only for the synchronous dispatch.
namespace D2RLInterop::NativeItemHookCompatV1 {
inline constexpr char ServiceName[]="d2rl-native-item-hook-compat";
inline constexpr std::uint32_t ServiceVersion=1;
inline constexpr std::uint64_t DropAudioRva=0x1C8130;
inline constexpr std::uint64_t AutomapUnitRva=0xD76E0;
using DropSoundReplacementFn=bool(__cdecl*)(void* unit,void* userData) noexcept;
using AutomapUnitObserverFn=void(__cdecl*)(void* unit,void* automapContext,void* userData) noexcept;
using RegisterDropFn=bool(__cdecl*)(
    const D2RL::PluginContext*,DropSoundReplacementFn,void*) noexcept;
using UnregisterDropFn=RegisterDropFn;
using RegisterAutomapFn=bool(__cdecl*)(
    const D2RL::PluginContext*,AutomapUnitObserverFn,void*) noexcept;
using UnregisterAutomapFn=RegisterAutomapFn;
struct Service {
    std::uint32_t size,version;
    std::uint64_t dropAudioRva,automapUnitRva;
    RegisterDropFn registerDrop;
    UnregisterDropFn unregisterDrop;
    RegisterAutomapFn registerAutomap;
    UnregisterAutomapFn unregisterAutomap;
};
inline bool Valid(const Service* s) noexcept {
    return s && s->size>=sizeof(Service) && s->version==ServiceVersion &&
        s->dropAudioRva==DropAudioRva && s->automapUnitRva==AutomapUnitRva &&
        s->registerDrop && s->unregisterDrop && s->registerAutomap &&
        s->unregisterAutomap;
}
static_assert(sizeof(Service)==56 && std::is_standard_layout_v<Service>);
}
