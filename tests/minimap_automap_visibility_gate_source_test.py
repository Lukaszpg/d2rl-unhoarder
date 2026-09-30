from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=(root/'src/plugin.cpp').read_text()
r=(root/'src/minimap_overlay_renderer.cpp').read_text()
h=(root/'src/minimap_overlay_renderer.hpp').read_text()

assert '.version = UNHOARDER_VERSION_STRING' in p
assert 'NativeUiStateTableRva=0x2A2ADA0' in p
assert 'NativeUiAutomapStateIndex=10' in p
assert 'NativeUiOpenStateWitnessRva=0x0CD7FB' in p
assert 'NativeUiCloseStateWitnessRva=0x0C7DF1' in p
assert 'NativeUiToggleStateWitnessRva=0x0CDE3C' in p
assert 'ExpectedNativeUiOpenStateWitness' in p
assert 'ExpectedNativeUiCloseStateWitness' in p
assert 'ExpectedNativeUiToggleStateWitness' in p
assert 'LOOT_MINIMAP_AUTOMAP_GATE_READY version=" UNHOARDER_VERSION_STRING "' in p
assert 'Base+NativeUiStateTableRva' in p
assert 'SetAutomapVisibilityTable' in h
assert 'NativeAutomapUiStateIndex = 10U' in r
assert 'IsNativeAutomapVisible()' in r
assert 'ClearPublishedFrameBestEffort();' in r
assert 'MarkerFrameFreshMilliseconds = 250U' in r
assert 'struct Diagnostics' not in h and 'GetDiagnostics' not in h and 'GetDiagnostics' not in r
print('1.0.0 native automap visibility gate / immediate close suppression: ok')
