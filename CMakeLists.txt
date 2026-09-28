# Build from the RuffnecKk-D2RLoader-Suite root (PluginSDK-v4 must be present).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    message(FATAL_ERROR "Install under <suite>/plugins/loot-filter and build from the suite root.")
endif()

include(FetchContent)

if(NOT TARGET minhook)
    FetchContent_Declare(
        minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
    )
    FetchContent_MakeAvailable(minhook)
endif()

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG f401021d5a5d56fe2304056c391e78f81c8d4b8f
)
FetchContent_MakeAvailable(imgui)

# Keep the SHARED plugin as the first library declaration so Charsi resolves
# the deployable DLL target rather than the private ImGui helper.
add_library(loot_filter SHARED
    src/plugin.cpp
    src/minimap_overlay_renderer.cpp
    src/plugin.rc
)

if(NOT TARGET loot_filter_imgui)
    add_library(loot_filter_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp"
    )
    target_include_directories(loot_filter_imgui PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
    )
    target_compile_features(loot_filter_imgui PUBLIC cxx_std_20)
    target_compile_definitions(loot_filter_imgui PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    )
    if(MSVC)
        target_compile_options(loot_filter_imgui PRIVATE /W3 /WX-)
    endif()
endif()

target_compile_features(loot_filter PRIVATE cxx_std_20)
target_compile_definitions(loot_filter PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(loot_filter PRIVATE
    D2RLPluginV4::D2RLPluginV4
    loot_filter_imgui
    minhook
    dxgi
    User32
)
set_target_properties(loot_filter PROPERTIES
    PREFIX ""
    OUTPUT_NAME "loot-filter"
)
if(MSVC)
    target_compile_options(loot_filter PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_options(loot_filter PRIVATE /INCREMENTAL:NO)
endif()

if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(loot_filter PRIVATE nlohmann_json::nlohmann_json)
elseif(EXISTS "${CMAKE_BINARY_DIR}/_deps/nlohmann_json-src/include/nlohmann/json.hpp")
    target_include_directories(loot_filter PRIVATE
        "${CMAKE_BINARY_DIR}/_deps/nlohmann_json-src/include")
else()
    message(FATAL_ERROR "Loot Filter 1.0.0 needs nlohmann_json headers from the D2RLoader Suite dependency")
endif()
