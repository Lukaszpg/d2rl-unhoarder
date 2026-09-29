from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
version = (root / "VERSION").read_text(encoding="utf-8").strip()
cm = (root / "CMakeLists.txt").read_text(encoding="utf-8")
plugin = (root / "src/plugin.cpp").read_text(encoding="utf-8")
renderer = (root / "src/minimap_overlay_renderer.cpp").read_text(encoding="utf-8")
rc = (root / "src/plugin.rc").read_text(encoding="utf-8")

assert re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version)
assert 'file(STRIP "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" UNHOARDER_VERSION)' in cm
assert 'VERSION "${UNHOARDER_VERSION}"' in cm
assert 'UNHOARDER_VERSION_STRING="${PROJECT_VERSION}"' in cm
assert '.version = UNHOARDER_VERSION_STRING' in plugin
assert 'version=" UNHOARDER_VERSION_STRING "' in plugin
assert 'version=" UNHOARDER_VERSION_STRING "' in renderer
assert 'FILEVERSION UNHOARDER_VERSION_MAJOR,UNHOARDER_VERSION_MINOR,UNHOARDER_VERSION_PATCH,0' in rc
assert 'VALUE "FileVersion", UNHOARDER_VERSION_STRING' in rc
assert 'VALUE "ProductVersion", UNHOARDER_VERSION_STRING' in rc
assert f"version={version}" not in plugin
assert f"version={version}" not in renderer
print(f"canonical VERSION source contract ok: {version}")
