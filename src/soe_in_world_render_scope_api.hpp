#pragma once
// Optional read-only ABI V3: same-thread lifetime of SoE's already hooked
// in-world unit-label builder. Not a draw event or a guarantee that a glyph
// belongs to the selected item. No owned native pointers may be retained.
#include <cstdint>
namespace SoE::Interop {
inline constexpr std::uint32_t InWorldRenderScopeAbiV3 = 3;
inline constexpr char InWorldRenderScopeExportName[] = "SoEGetInWorldRenderScopeInteropV3";
struct InWorldActiveItemV3 final {
    std::uint32_t structSize;
    std::int32_t unitType;
    std::uint32_t classId;
    std::uint32_t unitId;
    const void* nativeUnit; // borrowed only during synchronous same-thread call
};
struct InWorldRenderScopeApiV3 final {
    std::uint32_t structSize;
    std::uint32_t abiVersion;
    bool(__cdecl* isReady)() noexcept;
    bool(__cdecl* getCurrentItem)(InWorldActiveItemV3* out) noexcept;
};
static_assert(sizeof(InWorldActiveItemV3) == 24);
static_assert(sizeof(InWorldRenderScopeApiV3) == 24);
} // namespace SoE::Interop
