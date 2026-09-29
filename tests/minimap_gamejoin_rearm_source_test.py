from pathlib import Path
s=Path(__file__).resolve().parents[1].joinpath('src/plugin.cpp').read_text()
assert '.version = UNHOARDER_VERSION_STRING' in s
join=s[s.index('void __cdecl OnInWorldGameJoined'):s.index('void RegisterInWorldLifecycle')]
assert 'ResetMinimapTracking();' in join
assert 'LOOT_MINIMAP_PROBE_GAMEJOIN_REARM' not in join
assert join.index('ResetMinimapTracking();') < join.index('InitializeMinimapMarkerRenderer();') < join.index('TryAttachInWorldBackend();')
print('1.0.0 minimap tracking resets on GameJoined without probe logging: ok')
