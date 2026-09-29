from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=(root/'src/plugin.cpp').read_text(); r=(root/'src/minimap_overlay_renderer.cpp').read_text(); h=(root/'src/minimap_overlay_renderer.hpp').read_text(); cm=(root/'CMakeLists.txt').read_text(); rc=(root/'src/plugin.rc').read_text()
assert '.version = "1.0.0"' in p
assert 'FILEVERSION 1,0,0,0' in rc
assert 'sourceMarker=unhoarder-prod-v1' in p
assert 'MinimapOverlayRenderer::MarkerFrame markerFrame{};' in p
assert 'MinimapOverlayRenderer::Publish(markerFrame);' in p
assert 'ForgetMinimapProjectionItem(item->runtimeId);' in p
assert 'ForgetMinimapProjectionItem(info.runtimeId);' in p
join=p[p.index('void __cdecl OnInWorldGameJoined'):p.index('void RegisterInWorldLifecycle')]
assert join.index('ResetMinimapTracking();') < join.index('InitializeMinimapMarkerRenderer();') < join.index('TryAttachInWorldBackend();')
assert 'MarkerFrameFreshMilliseconds = 250U' in r
assert 'MinimapIconPolicy::MaximumSizePx' in r and 'const float radius=sizePx*0.5F;' in r
assert 'AddConvexPolyFilled' in r
assert 'RuffnecKkFloatingDamageGetOverlayApi' not in r and 'FloatingDamageHost' not in r
assert 'backend=standalone-d3d12' in r
assert 'HookPresent' in r and 'HookExecuteCommandLists' in r and 'HookResizeBuffers' in r
assert 'ImDrawData* const drawData = ImGui::GetDrawData();' in r
assert 'ImGui_ImplDX12_RenderDrawData(drawData, CommandList.Get());' in r
assert 'locbones authorized use, modification, and redistribution on 2026-08-16' in h
assert 'unhoarder_imgui' in cm and 'OUTPUT_NAME "unhoarder"' in cm
print('1.0.0 standalone JSON minimap marker/build contract ok')
