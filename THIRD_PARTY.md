# Third-party build dependencies

UnHoarder 1.0.0 fetches the following build dependencies at CMake configure time:

- D2RLoader PluginSDK v4, pinned through `Lukaszpg/RuffnecKk-D2RLoader-Suite` commit `697ed7fd2b767198e46c570cd9fd61e58a862bfc`, using only `third_party/PluginSDK-v4`.
- MinHook, commit `c3fcafdc10146beb5919319d0683e44e3c30d537`.
- Dear ImGui, commit `f401021d5a5d56fe2304056c391e78f81c8d4b8f`.
- nlohmann/json `v3.11.3`.

The dependencies retain their respective upstream licenses. Before publishing a binary/source release, include any notices required by the versions actually distributed.
