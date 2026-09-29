from pathlib import Path
root = Path(__file__).resolve().parents[1]
cm = (root / "CMakeLists.txt").read_text(encoding="utf-8")
workflow = (root / ".github/workflows/build.yml").read_text(encoding="utf-8")
assert "project(" in cm and "UnHoarder" in cm
assert "Install under <suite>" not in cm
assert "https://github.com/D2RLoader/PluginSDK.git" in cm
assert "717f727a0ec52912d1558764345f8fa3453a2bd6" in cm
assert "D2RLPlugin::D2RLPlugin" in cm
assert "RuffnecKk-D2RLoader-Suite.git" not in cm
assert "add_library(unhoarder SHARED" in cm
assert 'OUTPUT_NAME "unhoarder"' in cm
assert 'RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"' in cm
assert "plugins/unhoarder" not in workflow
assert "build\\bin\\unhoarder.dll" in workflow
print("1.0.0 standalone CMake/GitHub Actions build contract ok")
