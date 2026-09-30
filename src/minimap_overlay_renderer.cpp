#include "minimap_overlay_renderer.hpp"

#include <MinHook.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <vector>

namespace UnHoarder::MinimapOverlayRenderer {
namespace {
using Microsoft::WRL::ComPtr;

constexpr std::uint64_t MarkerFrameFreshMilliseconds = 250U;
constexpr std::size_t NativeAutomapUiStateIndex = 10U;
constexpr float MarkerStrokeWidth = 2.0F;
constexpr float StarInnerRadiusRatio = 0.45F;

// The loot filter owns this renderer outright. No MapSense or Floating Damage
// inter-plugin renderer API is required.

struct FrameContext final {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> renderTarget;
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor{};
    std::uint64_t fenceValue{};
};

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

HMODULE Module{};
std::array<void*, 150> Methods{};
PresentFn OriginalPresent{};
ExecuteCommandListsFn OriginalExecuteCommandLists{};
ResizeBuffersFn OriginalResizeBuffers{};

std::mutex RenderMutex;
std::mutex HookMutex;
std::mutex MarkerMutex;
MarkerFrame PublishedFrame{};

bool HooksInstalled{};
bool MinHookInitializedByRenderer{};
bool RendererInitialized{};
bool ImGuiContextCreated{};
bool ImGuiDx12Initialized{};
ImGuiContext* AutonomousImGuiContext{};
DXGI_FORMAT BackBufferFormat{DXGI_FORMAT_R8G8B8A8_UNORM};

struct RendererStorage final {
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent{};
    std::vector<FrameContext> frames;
};

// Intentionally process-lifetime storage. D2RLoader can terminate the process
// after D3D12 teardown without running plugin static destructors in a useful
// order; explicit unload still releases these resources through ResetRenderer.
RendererStorage* const ProcessRendererStorage = new RendererStorage{};
auto& CommandQueue = ProcessRendererStorage->commandQueue;
auto& CommandList = ProcessRendererStorage->commandList;
auto& RtvHeap = ProcessRendererStorage->rtvHeap;
auto& SrvHeap = ProcessRendererStorage->srvHeap;
auto& Fence = ProcessRendererStorage->fence;
auto& FenceEvent = ProcessRendererStorage->fenceEvent;
auto& Frames = ProcessRendererStorage->frames;
std::uint64_t NextFenceValue{1U};
std::atomic<ID3D12CommandQueue*> CapturedQueue{};
std::atomic<std::uint32_t> ActiveHookCalls{};
std::atomic<LogCallback> Logger{};
std::atomic<const volatile std::uint8_t*> AutomapVisibilityTable{};
std::atomic<std::uint32_t> DiagnosticMessages{};

enum class Backend : std::uint8_t {
    None,
    StandaloneD3D12,
};
std::atomic<Backend> ActiveBackend{Backend::None};

enum DiagnosticMessage : std::uint32_t {
    PresentInterceptedMessage = 1U << 0U,
    DirectQueueCapturedMessage = 1U << 1U,
    RendererInitializedMessage = 1U << 2U,
    FirstFrameRenderedMessage = 1U << 3U,
    RendererInitFailedMessage = 1U << 4U,
    FenceWaitFailedMessage = 1U << 5U,
    FenceSignalFailedMessage = 1U << 6U,
};

struct HookCallGuard final {
    HookCallGuard() noexcept {
        ActiveHookCalls.fetch_add(1U, std::memory_order_acq_rel);
    }
    ~HookCallGuard() {
        ActiveHookCalls.fetch_sub(1U, std::memory_order_acq_rel);
    }
    HookCallGuard(const HookCallGuard&) = delete;
    auto operator=(const HookCallGuard&) -> HookCallGuard& = delete;
};

void Log(const char* message) noexcept {
    if (const auto callback = Logger.load(std::memory_order_acquire)) {
        callback(message);
    }
}

void LogOnce(std::uint32_t bit, const char* message) noexcept {
    const auto previous = DiagnosticMessages.fetch_or(
        bit, std::memory_order_acq_rel);
    if ((previous & bit) == 0U) Log(message);
}

[[nodiscard]] bool IsNativeAutomapVisible() noexcept {
    const auto* const table = AutomapVisibilityTable.load(
        std::memory_order_acquire);
    if (table == nullptr) return false;
    __try {
        return table[NativeAutomapUiStateIndex] != 0U;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ClearPublishedFrameBestEffort() noexcept {
    if (!MarkerMutex.try_lock()) return;
    PublishedFrame = {};
    MarkerMutex.unlock();
}

[[nodiscard]] bool CopyFreshFrame(MarkerFrame& output) noexcept {
    output = {};
    // Native UI state is authoritative for visibility. This removes the old
    // 250 ms close-tail while retaining the freshness timeout as a secondary
    // safety net for stale projection data. Clearing here also prevents a fast
    // close/reopen from flashing the previous automap position.
    if (!IsNativeAutomapVisible()) {
        ClearPublishedFrameBestEffort();
        return false;
    }
    if (!MarkerMutex.try_lock()) return false;
    output = PublishedFrame;
    MarkerMutex.unlock();
    if (output.count == 0U || output.publishedTick == 0U) return false;
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    return now >= output.publishedTick
        && now - output.publishedTick <= MarkerFrameFreshMilliseconds;
}

[[nodiscard]] ImU32 ToImGuiColor(
        const std::array<float,4>& rgba) noexcept {
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(rgba[0],rgba[1],rgba[2],rgba[3]));
}

void DrawPolygonMarker(ImDrawList* drawList, const Marker& marker,
        const ImVec2* points, int count) noexcept {
    if (drawList == nullptr || points == nullptr || count < 3) return;
    drawList->AddConvexPolyFilled(points,count,ToImGuiColor(marker.fillColor));
    drawList->AddPolyline(points,count,ToImGuiColor(marker.borderColor),
        ImDrawFlags_Closed,MarkerStrokeWidth);
}

void DrawMarker(ImDrawList* drawList, const Marker& marker) noexcept {
    if (drawList == nullptr) return;
    const float sizePx=std::clamp(marker.sizePx,
        static_cast<float>(MinimapIconPolicy::MinimumSizePx),
        static_cast<float>(MinimapIconPolicy::MaximumSizePx));
    const float radius=sizePx*0.5F;
    switch(marker.shape) {
    case Shape::Circle:
        drawList->AddCircleFilled(
            ImVec2(marker.x,marker.y),radius,
            ToImGuiColor(marker.fillColor),24);
        drawList->AddCircle(
            ImVec2(marker.x,marker.y),radius,
            ToImGuiColor(marker.borderColor),24,MarkerStrokeWidth);
        return;
    case Shape::Triangle: {
        const ImVec2 points[3]{
            {marker.x,marker.y-radius},
            {marker.x+radius*0.8660254F,marker.y+radius*0.5F},
            {marker.x-radius*0.8660254F,marker.y+radius*0.5F},
        };
        DrawPolygonMarker(drawList,marker,points,3);
        return;
    }
    case Shape::Star: {
        std::array<ImVec2,10> points{};
        constexpr float pi=3.14159265358979323846F;
        for(std::size_t i=0;i<points.size();++i) {
            const float pointRadius=(i%2U)==0U ? radius :
                radius*StarInnerRadiusRatio;
            const float angle=-pi*0.5F+static_cast<float>(i)*pi/5.0F;
            points[i]={marker.x+std::cos(angle)*pointRadius,
                       marker.y+std::sin(angle)*pointRadius};
        }
        // A star is concave, so AddConvexPolyFilled is not valid. Fill it as
        // triangles from the center while retaining one closed outline.
        const auto fill=ToImGuiColor(marker.fillColor);
        for(std::size_t i=0;i<points.size();++i) {
            const ImVec2 tri[3]{
                {marker.x,marker.y},
                points[i],
                points[(i+1U)%points.size()]
            };
            drawList->AddTriangleFilled(tri[0],tri[1],tri[2],fill);
        }
        drawList->AddPolyline(points.data(),static_cast<int>(points.size()),
            ToImGuiColor(marker.borderColor),ImDrawFlags_Closed,
            MarkerStrokeWidth);
        return;
    }
    case Shape::Diamond:
    default: {
        const ImVec2 points[4]{
            {marker.x,marker.y-radius},
            {marker.x+radius,marker.y},
            {marker.x,marker.y+radius},
            {marker.x-radius,marker.y},
        };
        DrawPolygonMarker(drawList,marker,points,4);
        return;
    }
    }
}

void DrawMarkerFrameImGui(ImDrawList* drawList) noexcept {
    MarkerFrame frame{};
    if (!CopyFreshFrame(frame) || drawList == nullptr) return;
    if (frame.clip.right <= frame.clip.left
        || frame.clip.bottom <= frame.clip.top) {
        return;
    }
    drawList->PushClipRect(
        ImVec2(frame.clip.left, frame.clip.top),
        ImVec2(frame.clip.right, frame.clip.bottom), true);
    const auto count = std::min(frame.count, frame.markers.size());
    for (std::size_t index = 0; index < count; ++index) {
        DrawMarker(drawList, frame.markers[index]);
    }
    drawList->PopClipRect();
}

[[nodiscard]] bool WaitForFenceValueLocked(std::uint64_t value) noexcept {
    if (value == 0U || !Fence || !FenceEvent) return true;
    if (Fence->GetCompletedValue() >= value) return true;
    if (FAILED(Fence->SetEventOnCompletion(value, FenceEvent))) return false;
    return WaitForSingleObject(FenceEvent, 5'000U) == WAIT_OBJECT_0;
}

[[nodiscard]] bool WaitForGpuIdleLocked() noexcept {
    if (!CommandQueue || !Fence || !FenceEvent) return true;
    const auto value = NextFenceValue++;
    if (FAILED(CommandQueue->Signal(Fence.Get(), value))) return false;
    return WaitForFenceValueLocked(value);
}

void ResetRendererState() noexcept {
    if (!WaitForGpuIdleLocked()) {
        LogOnce(FenceWaitFailedMessage,
            "LOOT_MINIMAP_RENDERER_WARN version=" UNHOARDER_VERSION_STRING " backend=standalone-d3d12 reason=gpu-idle-timeout");
    }
    if (AutonomousImGuiContext != nullptr) {
        auto* const destroyedContext = AutonomousImGuiContext;
        auto* const previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(destroyedContext);
        if (ImGuiDx12Initialized) {
            ImGui_ImplDX12_Shutdown();
            ImGuiDx12Initialized = false;
        }
        if (ImGuiContextCreated) {
            ImGui::DestroyContext(destroyedContext);
        }
        ImGuiContextCreated = false;
        AutonomousImGuiContext = nullptr;
        if (previous != nullptr && previous != destroyedContext) {
            ImGui::SetCurrentContext(previous);
        } else {
            ImGui::SetCurrentContext(nullptr);
        }
    }
    RendererInitialized = false;
    Frames.clear();
    CommandList.Reset();
    RtvHeap.Reset();
    SrvHeap.Reset();
    Fence.Reset();
    if (FenceEvent != nullptr) {
        CloseHandle(FenceEvent);
        FenceEvent = nullptr;
    }
    CapturedQueue.store(nullptr, std::memory_order_release);
    CommandQueue.Reset();
    NextFenceValue = 1U;
}

void ResetRenderer() noexcept {
    std::scoped_lock lock(RenderMutex);
    ResetRendererState();
}

[[nodiscard]] bool FailRendererInitialization(
        std::uint32_t stage,
        const char* reason) noexcept {
    if ((DiagnosticMessages.fetch_or(
            RendererInitFailedMessage,
            std::memory_order_acq_rel) & RendererInitFailedMessage) == 0U) {
        char line[320]{};
        std::snprintf(line, sizeof(line),
            "LOOT_MINIMAP_RENDERER_INIT_FAILED version=" UNHOARDER_VERSION_STRING " backend=standalone-d3d12 stage=%u reason=%s",
            stage, reason ? reason : "unknown");
        Log(line);
    }
    ResetRendererState();
    return false;
}

[[nodiscard]] bool InitializeRenderer(IDXGISwapChain3* swapChain) noexcept {
    if (swapChain == nullptr) {
        return FailRendererInitialization(1U, "null-swap-chain");
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) {
        return FailRendererInitialization(2U, "swap-chain-device");
    }
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(swapChain->GetDesc(&swapDesc)) || swapDesc.BufferCount == 0U) {
        return FailRendererInitialization(3U, "swap-chain-desc");
    }
    BackBufferFormat = swapDesc.BufferDesc.Format == DXGI_FORMAT_UNKNOWN
        ? DXGI_FORMAT_R8G8B8A8_UNORM
        : swapDesc.BufferDesc.Format;

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 1U;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&SrvHeap)))) {
        return FailRendererInitialization(4U, "srv-heap");
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = swapDesc.BufferCount;
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&RtvHeap)))) {
        return FailRendererInitialization(5U, "rtv-heap");
    }

    Frames.clear();
    Frames.resize(swapDesc.BufferCount);
    auto descriptor = RtvHeap->GetCPUDescriptorHandleForHeapStart();
    const auto descriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT index = 0U; index < swapDesc.BufferCount; ++index) {
        auto& frame = Frames[index];
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&frame.allocator)))) {
            return FailRendererInitialization(6U, "command-allocator");
        }
        if (FAILED(swapChain->GetBuffer(index, IID_PPV_ARGS(&frame.renderTarget)))) {
            return FailRendererInitialization(7U, "back-buffer");
        }
        frame.descriptor = descriptor;
        device->CreateRenderTargetView(frame.renderTarget.Get(), nullptr, descriptor);
        descriptor.ptr += descriptorSize;
    }

    if (FAILED(device->CreateCommandList(
            0U, D3D12_COMMAND_LIST_TYPE_DIRECT,
            Frames[0].allocator.Get(), nullptr,
            IID_PPV_ARGS(&CommandList)))) {
        return FailRendererInitialization(8U, "command-list");
    }
    if (FAILED(CommandList->Close())) {
        return FailRendererInitialization(9U, "command-list-close");
    }
    if (FAILED(device->CreateFence(
            0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)))) {
        return FailRendererInitialization(10U, "fence");
    }
    FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FenceEvent == nullptr) {
        return FailRendererInitialization(11U, "fence-event");
    }

    IMGUI_CHECKVERSION();
    AutonomousImGuiContext = ImGui::CreateContext();
    if (AutonomousImGuiContext == nullptr) {
        return FailRendererInitialization(12U, "imgui-context");
    }
    ImGuiContextCreated = true;
    ImGui::SetCurrentContext(AutonomousImGuiContext);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefault();
    if (!ImGui_ImplDX12_Init(
            device.Get(), swapDesc.BufferCount, BackBufferFormat,
            SrvHeap.Get(),
            SrvHeap->GetCPUDescriptorHandleForHeapStart(),
            SrvHeap->GetGPUDescriptorHandleForHeapStart())) {
        return FailRendererInitialization(13U, "imgui-dx12-init");
    }
    ImGuiDx12Initialized = true;
    if (!ImGui_ImplDX12_CreateDeviceObjects()) {
        return FailRendererInitialization(14U, "imgui-device-objects");
    }
    RendererInitialized = true;
    LogOnce(RendererInitializedMessage,
        "LOOT_MINIMAP_RENDERER_DEVICE_READY version=" UNHOARDER_VERSION_STRING " backend=standalone-d3d12 imgui=1 dx12=1 markerRules=json-shape-border-fill-size");
    return true;
}

void RenderAutonomousFrameLocked(IDXGISwapChain3* swapChain) noexcept {
    MarkerFrame markerFrame{};
    if (!CopyFreshFrame(markerFrame)) return;
    if (!CommandQueue) return;
    if (!RendererInitialized && !InitializeRenderer(swapChain)) return;
    const UINT frameIndex = swapChain->GetCurrentBackBufferIndex();
    if (frameIndex >= Frames.size()) return;
    auto& frame = Frames[frameIndex];
    if (!WaitForFenceValueLocked(frame.fenceValue)) {
        LogOnce(FenceWaitFailedMessage,
            "LOOT_MINIMAP_RENDERER_WARN version=" UNHOARDER_VERSION_STRING " backend=standalone-d3d12 reason=allocator-still-in-use");
        return;
    }

    ImGui::SetCurrentContext(AutonomousImGuiContext);
    const auto resourceDesc = frame.renderTarget->GetDesc();
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(
        static_cast<float>(resourceDesc.Width),
        static_cast<float>(resourceDesc.Height));
    io.DeltaTime = 1.0F / 60.0F;
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    DrawMarkerFrameImGui(ImGui::GetForegroundDrawList());
    ImGui::Render();
    ImDrawData* const drawData = ImGui::GetDrawData();
    if (drawData == nullptr || drawData->TotalVtxCount <= 0) return;

    if (FAILED(frame.allocator->Reset())) return;
    if (FAILED(CommandList->Reset(frame.allocator.Get(), nullptr))) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.renderTarget.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    CommandList->ResourceBarrier(1U, &barrier);
    CommandList->OMSetRenderTargets(1U, &frame.descriptor, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[]{SrvHeap.Get()};
    CommandList->SetDescriptorHeaps(1U, heaps);
    ImGui_ImplDX12_RenderDrawData(drawData, CommandList.Get());
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    CommandList->ResourceBarrier(1U, &barrier);
    if (FAILED(CommandList->Close())) return;
    ID3D12CommandList* lists[]{CommandList.Get()};
    CommandQueue->ExecuteCommandLists(1U, lists);
    const auto fenceValue = NextFenceValue++;
    if (FAILED(CommandQueue->Signal(Fence.Get(), fenceValue))) {
        frame.fenceValue = (std::numeric_limits<std::uint64_t>::max)();
        LogOnce(FenceSignalFailedMessage,
            "LOOT_MINIMAP_RENDERER_WARN version=" UNHOARDER_VERSION_STRING " backend=standalone-d3d12 reason=fence-signal");
    } else {
        frame.fenceValue = fenceValue;
    }
    LogOnce(FirstFrameRenderedMessage,
        "LOOT_MINIMAP_RENDERER_FIRST_DRAW version=" UNHOARDER_VERSION_STRING " backend=standalone-d3d12 markerRules=json-shape-border-fill-size");
}

HRESULT STDMETHODCALLTYPE HookPresent(
        IDXGISwapChain3* swapChain,
        UINT syncInterval,
        UINT flags) noexcept {
    [[maybe_unused]] const HookCallGuard guard;
    LogOnce(PresentInterceptedMessage,
        "LOOT_MINIMAP_RENDERER_PRESENT_READY version=" UNHOARDER_VERSION_STRING " backend=standalone-d3d12");
    const auto original = OriginalPresent;
    if (original == nullptr) return DXGI_ERROR_INVALID_CALL;
    {
        std::scoped_lock lock(RenderMutex);
        RenderAutonomousFrameLocked(swapChain);
    }
    return original(swapChain, syncInterval, flags);
}

void STDMETHODCALLTYPE HookExecuteCommandLists(
        ID3D12CommandQueue* queue,
        UINT count,
        ID3D12CommandList* const* lists) noexcept {
    [[maybe_unused]] const HookCallGuard guard;
    const auto original = OriginalExecuteCommandLists;
    if (queue != nullptr
        && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT
        && CapturedQueue.load(std::memory_order_acquire) == nullptr) {
        std::scoped_lock lock(RenderMutex);
        if (!CommandQueue) {
            CommandQueue = queue;
            CapturedQueue.store(queue, std::memory_order_release);
            LogOnce(DirectQueueCapturedMessage,
                "LOOT_MINIMAP_RENDERER_QUEUE_READY version=" UNHOARDER_VERSION_STRING " backend=standalone-d3d12 source=first-direct-command-queue minimapIconRules=1");
        }
    }
    if (original != nullptr) original(queue, count, lists);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(
        IDXGISwapChain3* swapChain,
        UINT bufferCount,
        UINT width,
        UINT height,
        DXGI_FORMAT format,
        UINT flags) noexcept {
    [[maybe_unused]] const HookCallGuard guard;
    const auto original = OriginalResizeBuffers;
    ResetRenderer();
    return original != nullptr
        ? original(swapChain, bufferCount, width, height, format, flags)
        : DXGI_ERROR_INVALID_CALL;
}

[[nodiscard]] bool BuildMethodTable() noexcept {
    const wchar_t* className = L"UnHoarderMinimapBootstrap";
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = Module;
    windowClass.lpszClassName = className;
    if (!RegisterClassExW(&windowClass)
        && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    HWND bootstrapWindow = CreateWindowExW(
        0U, className, L"UnHoarder Minimap Overlay",
        WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
        nullptr, nullptr, Module, nullptr);
    if (bootstrapWindow == nullptr) return false;

    using D3D12CreateDeviceFn = HRESULT(WINAPI*)(
        IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE d3d12Module = GetModuleHandleW(L"d3d12.dll");
    if (d3d12Module == nullptr) d3d12Module = LoadLibraryW(L"d3d12.dll");
    const auto createDevice = d3d12Module != nullptr
        ? reinterpret_cast<D3D12CreateDeviceFn>(
            GetProcAddress(d3d12Module, "D3D12CreateDevice"))
        : nullptr;
    if (createDevice == nullptr) {
        DestroyWindow(bootstrapWindow);
        UnregisterClassW(className, Module);
        return false;
    }

    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<IDXGISwapChain> swapChain;
    bool success{};
    do {
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) break;
        if (FAILED(createDevice(
                nullptr, D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&device)))) break;
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(
                &queueDesc, IID_PPV_ARGS(&queue)))) break;
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&allocator)))) break;
        if (FAILED(device->CreateCommandList(
                0U, D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator.Get(), nullptr,
                IID_PPV_ARGS(&commandList)))) break;

        DXGI_SWAP_CHAIN_DESC swapDesc{};
        swapDesc.BufferDesc.Width = 100U;
        swapDesc.BufferDesc.Height = 100U;
        swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.SampleDesc.Count = 1U;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2U;
        swapDesc.OutputWindow = bootstrapWindow;
        swapDesc.Windowed = TRUE;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChain(
                queue.Get(), &swapDesc, &swapChain))) break;

        std::memcpy(Methods.data(),
            *reinterpret_cast<void***>(device.Get()), 44U * sizeof(void*));
        std::memcpy(Methods.data() + 44U,
            *reinterpret_cast<void***>(queue.Get()), 19U * sizeof(void*));
        std::memcpy(Methods.data() + 63U,
            *reinterpret_cast<void***>(allocator.Get()), 9U * sizeof(void*));
        std::memcpy(Methods.data() + 72U,
            *reinterpret_cast<void***>(commandList.Get()), 60U * sizeof(void*));
        std::memcpy(Methods.data() + 132U,
            *reinterpret_cast<void***>(swapChain.Get()), 18U * sizeof(void*));
        success = true;
    } while (false);

    DestroyWindow(bootstrapWindow);
    UnregisterClassW(className, Module);
    return success;
}

[[nodiscard]] bool CreateHook(
        std::size_t methodIndex,
        void* target,
        void** original) noexcept {
    void* const address = Methods[methodIndex];
    if (address == nullptr) return false;
    if (MH_CreateHook(address, target, original) != MH_OK) return false;
    if (MH_EnableHook(address) == MH_OK) return true;
    MH_RemoveHook(address);
    if (original != nullptr) *original = nullptr;
    return false;
}

[[nodiscard]] bool InstallStandaloneHooks() noexcept {
    std::scoped_lock hookLock(HookMutex);
    if (HooksInstalled) return true;
    if (Module == nullptr || !BuildMethodTable()) return false;
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        return false;
    }
    if (initialized == MH_OK) MinHookInitializedByRenderer = true;
    const auto fail = []() noexcept {
        OriginalResizeBuffers = nullptr;
        OriginalPresent = nullptr;
        OriginalExecuteCommandLists = nullptr;
        Methods = {};
        if (MinHookInitializedByRenderer) {
            MH_Uninitialize();
            MinHookInitializedByRenderer = false;
        }
        return false;
    };
    if (!CreateHook(
            54U, reinterpret_cast<void*>(&HookExecuteCommandLists),
            reinterpret_cast<void**>(&OriginalExecuteCommandLists))) {
        return fail();
    }
    if (!CreateHook(
            140U, reinterpret_cast<void*>(&HookPresent),
            reinterpret_cast<void**>(&OriginalPresent))) {
        MH_DisableHook(Methods[54U]);
        MH_RemoveHook(Methods[54U]);
        return fail();
    }
    if (!CreateHook(
            145U, reinterpret_cast<void*>(&HookResizeBuffers),
            reinterpret_cast<void**>(&OriginalResizeBuffers))) {
        MH_DisableHook(Methods[140U]);
        MH_RemoveHook(Methods[140U]);
        MH_DisableHook(Methods[54U]);
        MH_RemoveHook(Methods[54U]);
        return fail();
    }
    HooksInstalled = true;
    ActiveBackend.store(Backend::StandaloneD3D12, std::memory_order_release);
    Log("LOOT_MINIMAP_RENDERER_READY version=" UNHOARDER_VERSION_STRING " backend=standalone-d3d12 presentHook=1 queueHook=1 resizeHook=1 markerRules=json-shape-border-fill-size sizePx=json-default12-range12..40-clamped automapGate=native-ui-state-10 mapSenseDependency=0");
    return true;
}

void RemoveStandaloneHooks() noexcept {
    std::scoped_lock hookLock(HookMutex);
    if (HooksInstalled) {
        MH_DisableHook(Methods[145U]);
        MH_DisableHook(Methods[140U]);
        MH_DisableHook(Methods[54U]);
        while (ActiveHookCalls.load(std::memory_order_acquire) != 0U) {
            Sleep(1U);
        }
    }
    ResetRenderer();
    if (HooksInstalled) {
        MH_RemoveHook(Methods[145U]);
        MH_RemoveHook(Methods[140U]);
        MH_RemoveHook(Methods[54U]);
    }
    HooksInstalled = false;
    OriginalResizeBuffers = nullptr;
    OriginalPresent = nullptr;
    OriginalExecuteCommandLists = nullptr;
    Methods = {};
    if (MinHookInitializedByRenderer) {
        MH_Uninitialize();
        MinHookInitializedByRenderer = false;
    }
}

} // namespace

void SetDllModule(HMODULE module) noexcept {
    Module = module;
}

void SetLogCallback(LogCallback callback) noexcept {
    Logger.store(callback, std::memory_order_release);
}

void SetAutomapVisibilityTable(
        const volatile std::uint8_t* table) noexcept {
    AutomapVisibilityTable.store(table, std::memory_order_release);
}

bool Initialize() noexcept {
    if (ActiveBackend.load(std::memory_order_acquire) != Backend::None) {
        return true;
    }
    if (InstallStandaloneHooks()) return true;
    Log("LOOT_MINIMAP_RENDERER_REFUSED version=" UNHOARDER_VERSION_STRING " backend=none reason=standalone-hook-install-failed projectionContinues=1 drawing=0 mapSenseDependency=0 floatingDamageDependency=0");
    return false;
}

void Shutdown() noexcept {
    Clear();
    AutomapVisibilityTable.store(nullptr, std::memory_order_release);
    if (HooksInstalled) RemoveStandaloneHooks();
    ActiveBackend.store(Backend::None, std::memory_order_release);
}

void Publish(const MarkerFrame& frame) noexcept {
    if (!MarkerMutex.try_lock()) return;
    PublishedFrame = frame;
    MarkerMutex.unlock();
}

void Clear() noexcept {
    if (!MarkerMutex.try_lock()) return;
    PublishedFrame = {};
    MarkerMutex.unlock();
}

const char* ActiveBackendName() noexcept {
    switch (ActiveBackend.load(std::memory_order_acquire)) {
        case Backend::StandaloneD3D12: return "standalone-d3d12";
        case Backend::None: return "none";
    }
    return "none";
}

} // namespace UnHoarder::MinimapOverlayRenderer
