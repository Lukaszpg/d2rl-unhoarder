from pathlib import Path
cm = Path(__file__).parents[1].joinpath('CMakeLists.txt').read_text(encoding='utf-8')
plugin = cm.index('add_library(loot_filter SHARED')
helper = cm.index('add_library(loot_filter_imgui STATIC')
assert plugin < helper, 'SHARED plugin target must be first library declaration for Charsi metadata resolution'
assert 'OUTPUT_NAME "loot-filter"' in cm
print('1.0.0 Charsi target selection: shared plugin precedes static ImGui helper: ok')
