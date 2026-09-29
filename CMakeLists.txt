cmake_minimum_required(VERSION 3.28)

project(
    UnHoarder
    VERSION 1.0.0
    DESCRIPTION "Item filter for Diablo II: Resurrected"
    LANGUAGES CXX RC
)

include(FetchContent)

option(
    UNHOARDER_WARNINGS_AS_ERRORS
    "Treat UnHoarder compiler warnings as errors"
    OFF
)

set(
    UNHOARDER_PLUGINSDK_V4_SOURCE_DIR "" CACHE PATH
    "Optional local D2RLoader PluginSDK-v4 source directory; when empty, fetch the pinned SDK snapshot"
)

# ---------------------------------------------------------------------------
# D2RLoader PluginSDK v4
# ---------------------------------------------------------------------------
# The runtime dependency is D2RLoader itself, but UnHoarder no longer needs to
# live inside or configure the RuffnecKk Suite.  For reproducible standalone
# builds we pin the exact PluginSDK-v4 snapshot used for UnHoarder 1.0.0.
if(UNHOARDER_PLUGINSDK_V4_SOURCE_DIR)
    if(NOT EXISTS "${UNHOARDER_PLUGINSDK_V4_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "UNHOARDER_PLUGINSDK_V4_SOURCE_DIR does not contain CMakeLists.txt: "
            "${UNHOARDER_PLUGINSDK_V4_SOURCE_DIR}"
        )
    endif()
    add_subdirectory(
        "${UNHOARDER_PLUGINSDK_V4_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/_deps/d2r_plugin_sdk-build"
    )
else()
    FetchContent_Declare(
        d2r_plugin_sdk
        GIT_REPOSITORY https://github.com/Lukaszpg/RuffnecKk-D2RLoader-Suite.git
        GIT_TAG 697ed7fd2b767198e46c570cd9fd61e58a862bfc
        SOURCE_SUBDIR third_party/PluginSDK-v4
    )
    FetchContent_MakeAvailable(d2r_plugin_sdk)
endif()

if(NOT TARGET D2RLPluginV4::D2RLPluginV4)
    message(FATAL_ERROR "D2RLoader PluginSDK-v4 target D2RLPluginV4::D2RLPluginV4 is unavailable")
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
)

target_link_libraries(unhoarder PRIVATE
    D2RLPluginV4::D2RLPluginV4
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
