# Third-party build dependencies

UnHoarder fetches the following build dependencies at CMake configure time:

- D2RLoader PluginSDK 0.3.0 / ABI 4, fetched directly from `D2RLoader/PluginSDK` and pinned to commit `717f727a0ec52912d1558764345f8fa3453a2bd6` (MIT).
- MinHook, commit `c3fcafdc10146beb5919319d0683e44e3c30d537`.
- Dear ImGui, commit `f401021d5a5d56fe2304056c391e78f81c8d4b8f`.
- nlohmann/json `v3.11.3`.

The dependencies retain their respective upstream licenses. Before publishing a binary/source release, include any notices required by the versions actually distributed.
