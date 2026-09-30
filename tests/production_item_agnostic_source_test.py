from pathlib import Path
root=Path(__file__).resolve().parents[1]
plugin=(root/'src/plugin.cpp').read_text()
renderer=(root/'src/minimap_overlay_renderer.cpp').read_text()
sound=(root/'src/ground_sound_registry.hpp').read_text()
for forbidden in [
    'DivineCode','PurpleDivineBackground','ProbeCyanGlyphColor','GroundTextCyan',
    'GroundDivineGlyphSample','GroundMapGlyphSample','PackFilterCode("mp04")',
    'BackgroundPaintLastDivine','BackgroundPaintLastMap','BackgroundPaintObserveArmed',
    'ReportGroundGlyphStatus','ReportBackgroundPaintStatus','ReportCorrectedGlyphB',
    'ReportInWorldStatus','ReportGroundSoundStatus','InWorldSample','RecordInWorldSample',
    'IdentityProbeSlots',
]:
    assert forbidden not in plugin, forbidden
assert 'Divine Orb' not in plugin and 'Exalted Orb' not in plugin
assert 'IdentityLookupWindow' in plugin
assert 'EnsureSharedLabelPaintHook' in plugin
assert 'CollisionScanLimit' in sound and 'ProbeCount' not in sound
assert 'UnHoarderMinimapBootstrap' in renderer
# Probe-era filenames remain only as intentional migration/conflict compatibility.
assert 'L"loot-filter-probe.json"' in plugin
assert 'GetModuleHandleW(L"loot-filter-probe.dll")' in plugin
print('production source is item-agnostic; only explicit legacy probe-name compatibility remains')
