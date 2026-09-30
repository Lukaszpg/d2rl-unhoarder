from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=(root/'src/plugin.cpp').read_text()
r=(root/'src/minimap_overlay_renderer.cpp').read_text()
h=(root/'src/minimap_overlay_renderer.hpp').read_text()
policy=(root/'src/minimap_icon_policy.hpp').read_text()
e=(root/'unhoarder.v3.example.json').read_text()

assert '.version = UNHOARDER_VERSION_STRING' in p
assert 'bool hasMinimapIcon{};' in p
assert 'std::size_t minimapIconRules{};' in p
assert 'block->contains("minimapIcon") && !(*block)["minimapIcon"].is_object()' in p
assert 'icon.size()!=3U && icon.size()!=4U' in p
for field in ('shape','borderColor','fillColor'):
    assert f'icon.contains("{field}")' in p
assert 'const bool hasSize=icon.contains("size");' in p
assert 'icon["size"].is_number_integer()' in p
assert 'key.key()!="fillColor" && key.key()!="size"' in p
assert 'expected=circle|diamond|triangle|star' in p
assert 'ParseFilterRgba(icon["borderColor"]' in p
assert 'ParseFilterRgba(icon["fillColor"]' in p
assert 'MinimapIconPolicy::TryNormalizeSizePx' in p
assert 'clamped-to-12..40' in p
assert 'float minimapSizePx{MinimapIconPolicy::DefaultSizePx};' in p
assert 'item.sizePx=rule->minimapSizePx;' in p
assert 'marker.sizePx=item.sizePx;' in p
assert '++fresh->minimapIconRules;' in p
assert 'UpdateMinimapProjectionIconRule(header[2],code,rule);' in p
assert 'ClearMinimapProjectionIconStyles();' in p

for shape in ('Circle','Diamond','Triangle','Star'):
    assert f'{shape},' in h or f'{shape}\n' in h
assert 'std::array<float,4> borderColor' in h
assert 'std::array<float,4> fillColor' in h
assert 'float sizePx{MinimapIconPolicy::DefaultSizePx};' in h
assert 'MinimapIconPolicy::MinimumSizePx' in r and 'MinimapIconPolicy::MaximumSizePx' in r
assert 'const float radius=sizePx*0.5F;' in r
assert 'AddCircleFilled' in r
assert 'AddTriangleFilled' in r
assert 'DrawPolygonMarker' in r
assert 'ToImGuiColor(marker.borderColor)' in r
assert 'ToImGuiColor(marker.fillColor)' in r

assert 'MinimumSizePx = 12' in policy
assert 'DefaultSizePx = 12.0F' in policy
assert 'MaximumSizePx = 40' in policy
assert 'requested < MinimumSizePx' in policy
assert 'requested > MaximumSizePx' in policy

assert '"minimapIcon"' in e
assert '"shape": "diamond"' in e
assert '"borderColor": "RGBA(' in e
assert '"fillColor": "RGBA(' in e
assert '"size": 24' in e
print('JSON minimapIcon shape/border/fill/size source contract ok')
