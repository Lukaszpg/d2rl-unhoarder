from pathlib import Path

repo = Path(__file__).resolve().parents[1]
version = (repo / "VERSION").read_text().strip()
cmake = (repo / "CMakeLists.txt").read_text()
source = (repo / "src/plugin.cpp").read_text()
script = (repo / "cmake/write_build_commit.cmake").read_text()
assert version == "1.2.2", "UnHoarder local version must not inherit upstream 1.2.19"
assert 'file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"' in cmake
assert 'UNHOARDER_VERSION_STRING="${PROJECT_VERSION}"' in cmake
assert 'add_custom_target(unhoarder-build-commit' in cmake
assert 'add_dependencies(unhoarder unhoarder-build-commit)' in cmake
assert 'target_include_directories(unhoarder PRIVATE "${UNHOARDER_BUILD_INFO_DIR}")' in cmake
assert 'rev-parse --verify HEAD' in script
assert 'string(SUBSTRING "${gitHash}" 0 12 commit)' in script
assert 'if(existing STREQUAL contents)' in script
assert 'set(commit "unknown")' in script
assert '#include "unhoarder_build_commit.hpp"' in source
assert '"LOOT_COMPAT_START version=" UNHOARDER_VERSION_STRING' in source
assert '" commit=" UNHOARDER_BUILD_COMMIT_STRING " targetBuild=93847 "' in source
print("UnHoarder startup version and fresh build-time Git revision are source-grounded.")
