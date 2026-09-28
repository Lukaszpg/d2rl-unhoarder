#pragma once
// Optional V2 external style transformation. V1 remains read-only and binary
// compatible. V2 runs synchronously on SoE's qualified in-world ITEM relay,
// not on inventory tooltips or portal objects. All pointers are borrowed.
#include "soe_in_world_label_api.hpp"

namespace SoE::Interop {
inline constexpr std::uint32_t InWorldLabelStyleAbiV2 = 2;
inline constexpr char InWorldLabelStyleExportName[] = "SoEGetInWorldLabelStyleInteropV2";
using InWorldLabelStyleCallbackV2 = bool(__cdecl*)(
    const InWorldLabelEventV1* event, char* replacement,
    std::uint32_t capacity, void* userData) noexcept;
struct InWorldLabelStyleApiV2 final {
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    bool(__cdecl* isReady)() noexcept;
    bool(__cdecl* registerTransformer)(
        const char* owner, InWorldLabelStyleCallbackV2 callback, void* userData) noexcept;
    bool(__cdecl* unregisterTransformer)(
        const char* owner, InWorldLabelStyleCallbackV2 callback, void* userData) noexcept;
};
static_assert(sizeof(InWorldLabelStyleApiV2) == 32);
} // namespace SoE::Interop
