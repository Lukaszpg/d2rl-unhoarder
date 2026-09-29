cmake_minimum_required(VERSION 3.29)

file(STRIP "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" UNHOARDER_VERSION)
if(NOT UNHOARDER_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain MAJOR.MINOR.PATCH, got: '${UNHOARDER_VERSION}'")
endif()

project(
    UnHoarder
    VERSION "${UNHOARDER_VERSION}"
    DESCRIPTION "Item filter for Diablo II: Resurrected"
    LANGUAGES CXX RC
)

include(FetchContent)

option(
    UNHOARDER_WARNINGS_AS_ERRORS
    "Treat UnHoarder compiler warnings as errors"
    OFF
)
option(
    UNHOARDER_BUILD_TESTS
    "Build and register UnHoarder's portable C++ tests"
    OFF
)

set(
    UNHOARDER_PLUGINSDK_SOURCE_DIR "" CACHE PATH
    "Optional local D2RLoader PluginSDK source directory; when empty, fetch the pinned official SDK snapshot"
)

# ---------------------------------------------------------------------------
# D2RLoader PluginSDK
# ---------------------------------------------------------------------------
# UnHoarder consumes the public standalone SDK directly from D2RLoader/PluginSDK.
# Pinning the commit keeps CI and local builds reproducible while avoiding any
# build-time dependency on the RuffnecKk Suite repository.
set(BUILD_TESTING OFF CACHE BOOL "Disable dependency CTest targets in UnHoarder builds" FORCE)
set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "Do not build PluginSDK examples for UnHoarder" FORCE)
set(D2RLPLUGIN_BUILD_EXAMPLES_BY_DEFAULT OFF CACHE BOOL "Do not build PluginSDK examples by default" FORCE)

if(UNHOARDER_PLUGINSDK_SOURCE_DIR)
    if(NOT EXISTS "${UNHOARDER_PLUGINSDK_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "UNHOARDER_PLUGINSDK_SOURCE_DIR does not contain CMakeLists.txt: "
            "${UNHOARDER_PLUGINSDK_SOURCE_DIR}"
        )
    endif()
    add_subdirectory(
        "${UNHOARDER_PLUGINSDK_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/_deps/d2r_plugin_sdk-build"
        EXCLUDE_FROM_ALL
    )
else()
    FetchContent_Declare(
        d2r_plugin_sdk
        GIT_REPOSITORY https://github.com/D2RLoader/PluginSDK.git
        GIT_TAG 717f727a0ec52912d1558764345f8fa3453a2bd6
    )
    FetchContent_MakeAvailable(d2r_plugin_sdk)
endif()

if(NOT TARGET D2RLPlugin::D2RLPlugin)
    message(FATAL_ERROR "D2RLoader PluginSDK target D2RLPlugin::D2RLPlugin is unavailable")
endif()

# ---------------------------------------------------------------------------
# MinHook
# ---------------------------------------------------------------------------
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
)
FetchContent_MakeAvailable(minhook)

# ---------------------------------------------------------------------------
# Dear ImGui
# ---------------------------------------------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG f401021d5a5d56fe2304056c391e78f81c8d4b8f
)
FetchContent_MakeAvailable(imgui)

add_library(unhoarder_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp"
)

target_include_directories(unhoarder_imgui PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)

target_compile_features(unhoarder_imgui PUBLIC cxx_std_20)
target_compile_definitions(unhoarder_imgui PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(unhoarder_imgui PRIVATE /W3 /WX-)
endif()

# ---------------------------------------------------------------------------
# nlohmann/json
# ---------------------------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# UnHoarder
# ---------------------------------------------------------------------------
add_library(unhoarder SHARED
    src/plugin.cpp
    src/minimap_overlay_renderer.cpp
    src/plugin.rc
)

target_compile_features(unhoarder PRIVATE cxx_std_20)
target_compile_definitions(unhoarder PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNHOARDER_VERSION_STRING="${PROJECT_VERSION}"
    UNHOARDER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    UNHOARDER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    UNHOARDER_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_link_libraries(unhoarder PRIVATE
    D2RLPlugin::D2RLPlugin
    nlohmann_json::nlohmann_json
    unhoarder_imgui
    minhook
    dxgi
    User32
)

set_target_properties(unhoarder PROPERTIES
    PREFIX ""
    OUTPUT_NAME "unhoarder"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(unhoarder PRIVATE
        /W4
        /permissive-
        /utf-8
        /EHsc
    )
    if(UNHOARDER_WARNINGS_AS_ERRORS)
        target_compile_options(unhoarder PRIVATE /WX)
    else()
        target_compile_options(unhoarder PRIVATE /WX-)
    endif()
    target_link_options(unhoarder PRIVATE /INCREMENTAL:NO)
endif()


# ---------------------------------------------------------------------------
# Portable tests
# ---------------------------------------------------------------------------
if(UNHOARDER_BUILD_TESTS)
    enable_testing()
    file(GLOB UNHOARDER_PORTABLE_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp"
    )
    foreach(test_source IN LISTS UNHOARDER_PORTABLE_TEST_SOURCES)
        get_filename_component(test_name "${test_source}" NAME_WE)
        add_executable("${test_name}" "${test_source}")
        target_compile_features("${test_name}" PRIVATE cxx_std_20)
        target_include_directories("${test_name}" PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        if(MSVC)
            target_compile_options("${test_name}" PRIVATE /W4 /permissive- /utf-8 /EHsc /WX-)
        endif()
        add_test(NAME "${test_name}" COMMAND "${test_name}")
    endforeach()
endif()
