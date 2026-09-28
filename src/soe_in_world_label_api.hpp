#pragma once

// SoE's optional inter-plugin in-world-label observation ABI. This C ABI is
// intentionally independent of PluginSDK C++ class names and SoE gameplay.
// V1 is READ-ONLY. Do not retain event pointers beyond the callback.
#include <cstdint>

namespace SoE::Interop {
inline constexpr std::uint32_t InWorldLabelAbiV1 = 1;
inline constexpr char InWorldLabelExportName[] = "SoEGetInWorldLabelInteropV1";

struct InWorldLabelEventV1 final {
    std::uint32_t structSize;
    std::uint32_t flags; // zero in V1
    std::int32_t unitType;
    std::uint32_t classId;
    std::uint32_t unitId;
    std::uint32_t sourceLength; // length without NUL; native formatting bytes preserved
    const void* nativeUnit; // borrowed; valid ONLY during callback
    const char* source; // borrowed; sourceLength bytes followed by NUL
};
using InWorldLabelCallbackV1 = void(__cdecl*)(
    const InWorldLabelEventV1*, void* userData) noexcept;
struct InWorldLabelApiV1 final {
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    bool(__cdecl* isReady)() noexcept;
    bool(__cdecl* registerObserver)(
        const char* owner, InWorldLabelCallbackV1 callback, void* userData) noexcept;
    bool(__cdecl* unregisterObserver)(
        const char* owner, InWorldLabelCallbackV1 callback, void* userData) noexcept;
};
static_assert(sizeof(InWorldLabelEventV1) == 40);
static_assert(sizeof(InWorldLabelApiV1) == 32);
} // namespace SoE::Interop
