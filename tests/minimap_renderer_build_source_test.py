from pathlib import Path
root=Path(__file__).resolve().parents[1]
r=(root/'src/minimap_overlay_renderer.cpp').read_text()
h=(root/'src/minimap_overlay_renderer.hpp').read_text()

assert 'ImDrawData* const drawData = ImGui::GetDrawData();' in r
assert 'const auto* drawData = ImGui::GetDrawData();' not in r
assert 'ImGui_ImplDX12_RenderDrawData(drawData, CommandList.Get());' in r
assert 'backend=standalone-d3d12' in r
assert 'No external\n// renderer host is required' in h
print('renderer MSVC ABI fix and standalone-only contract ok')
