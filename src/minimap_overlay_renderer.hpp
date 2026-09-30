#pragma once

// Standalone D3D12/ImGui renderer used only for UnHoarder automap markers.
// The autonomous DX12 interception path is a reduced derivative of the
// Floating Damage renderer in RuffnecKk-D2RLoader-Suite, which in turn is
// derived from locbones/D2RHUD-2.4 at b9373f8508282948ceb3e2b56f892d9eba475744.
// locbones authorized use, modification, and redistribution on 2026-08-16.
// Keep this notice with derivative source.

#include <Windows.h>

#include "minimap_icon_policy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace UnHoarder::MinimapOverlayRenderer {

inline constexpr std::size_t MaximumMarkers = 64U;

enum class Shape : std::uint8_t {
    Circle,
    Diamond,
    Triangle,
    Star,
};

struct Marker final {
    float x{};
    float y{};
    std::uint32_t unitId{};
    Shape shape{Shape::Diamond};
    std::array<float,4> borderColor{1.0F,1.0F,1.0F,1.0F};
    std::array<float,4> fillColor{1.0F,1.0F,1.0F,1.0F};
    float sizePx{MinimapIconPolicy::DefaultSizePx};
};

struct ClipRect final {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

struct MarkerFrame final {
    std::array<Marker, MaximumMarkers> markers{};
    std::size_t count{};
    ClipRect clip{};
    std::uint64_t publishedTick{};
    std::uint64_t sequence{};
};

struct Diagnostics final {
    std::uint64_t presentCalls{};
    std::uint64_t directQueueCaptures{};
    std::uint64_t rendererInitAttempts{};
    std::uint64_t rendererInitFailures{};
    std::uint64_t renderedFrames{};
    std::uint64_t publishedFrames{};
    std::uint64_t drawnMarkers{};
    std::uint64_t automapSuppressedFrames{};
    std::uint32_t lastInitFailureStage{};
    bool hooksInstalled{};
    bool commandQueueReady{};
    bool rendererInitialized{};
};

using LogCallback = void(__cdecl*)(const char* message) noexcept;

void SetDllModule(HMODULE module) noexcept;
void SetLogCallback(LogCallback callback) noexcept;
// Qualified D2R UI-state table. Entry 10 is the native automap-open state.
// The renderer reads only that byte during Present and retains no UI pointer.
void SetAutomapVisibilityTable(const volatile std::uint8_t* table) noexcept;

// Installs the UnHoarder-owned standalone renderer. MapSense ownership is
// checked by the native projection layer before this is called. No external
// renderer host is required by this module.
[[nodiscard]] bool Initialize() noexcept;
void Shutdown() noexcept;

// Copies one complete latest marker frame. No borrowed D2R pointer or
// AutomapContext escapes the native projection callback.
void Publish(const MarkerFrame& frame) noexcept;
void Clear() noexcept;

[[nodiscard]] Diagnostics GetDiagnostics() noexcept;
[[nodiscard]] const char* ActiveBackendName() noexcept;

} // namespace UnHoarder::MinimapOverlayRenderer
