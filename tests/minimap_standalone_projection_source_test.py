from pathlib import Path
s=(Path(__file__).resolve().parents[1]/"src/plugin.cpp").read_text()
assert '.version = "1.0.0"' in s
assert 'sourceMarker=loot-filter-prod-v1' in s
for term in ('AutomapRenderUnitRva=0xD76E0','ProjectClientToAutomapRva=0xD4910','GetLocalDataContextRva=0x8B2D0','GetLocalPlayerRva=0x9A480','ExpectedAutomapRenderUnit','ExpectedProjectClientToAutomap','ExpectedGetLocalDataContext','ExpectedGetLocalPlayer'):
    assert term in s,term
assert 'GetModuleHandleW(L"d2rl-ruffneckk-mapsense.dll")' in s
assert 'reason=mapsense-loaded-shared-rendezvous' in s
assert 'InstallInlineHook(AutomapRenderUnitRva' in s
assert 'original(unit,automapContext);' in s
assert 'ProjectClientToAutomap(automapContext,&projected' in s
assert 'AutomapProjectionProbe::WorldSubtileToClient' in s
assert 'AutomapProjectionProbe::PackClientCoordinates' in s
assert 'AutomapProjectionProbe::Contains(clip,screen)' in s
assert 'LOOT_MINIMAP_PROJECTION_READY version=1.0.0' in s
assert 'mapSenseDependency=0 standalone=1 coexistence=fail-closed-if-mapsense-loaded' in s
assert 'InstallAutomapPayloadObserver' not in s
print('1.0.0 standalone native automap projection production contract ok')
