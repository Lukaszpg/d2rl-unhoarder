# UnHoarder development

Preserve exact native admission, provider ownership and fail-open behavior.
The independent fork builds without Controller QOL; optional services retain
their standalone fallbacks. See [packaging](docs/PACKAGING.md) and the feature
contracts under [docs](docs/).

## Building from source

Requirements:

- Windows x64
- CMake 3.29+
- MSVC with C++20 support
- Git available to CMake for pinned `FetchContent` dependencies

Configure and build:

```powershell
cmake -S . -B build
cmake --build build --config Release --target unhoarder --parallel
```

The resulting plugin is:

```text
build/bin/unhoarder.dll
```

Copy it to your mod's D2RLoader plugin directory, for example:

```text
<Diablo II Resurrected>/mods/<mod>/d2rloader/plugins/unhoarder.dll
```

Place `filter.json` in the same mod's `d2rloader/config` directory.

### PluginSDK source

By default CMake fetches the public D2RLoader PluginSDK directly from `D2RLoader/PluginSDK`, pinned to commit `717f727a0ec52912d1558764345f8fa3453a2bd6` (SDK 0.3.0 / plugin ABI 4) for reproducible builds.

For an offline/local SDK checkout, configure with:

```powershell
cmake -S . -B build `
  -G "Visual Studio 17 2022" -A x64 `
  -DUNHOARDER_PLUGINSDK_SOURCE_DIR="../PluginSDK"
```

## Plugin interoperability

UnHoarder uses D2RLoader `DiagnosticsService` to identify the **actual owner**
of a shared native boundary and `PluginCommunicationService` to cooperate
with that exact provider. It never enables compatibility from a DLL name,
load order, private export, or guessed foreign detour.

Four versioned native-hook/provider contracts are currently defined (in addition to UnHoarder's ground and label visibility services):

| Contract | Native boundary | Role in UnHoarder |
| --- | ---: | --- |
| `unhoarder-tooltip-compat` v1 | `0x1FA8E0`, `0x658510` | host or consumer for shared label-paint / glyph-render middleware |
| `unit-stat-read-compat` v1 | `0x2F5020` | consumer when another plugin owns the qualified UnitStat bridge |
| `in-world-item-label-compat` v1 | `0xC0420` | consumer when another plugin owns the hidden-hover item-label path |
| `d2rl-native-item-hook-compat` v1 | `0x1C8130`, `0xD76E0` | host or owner-verified consumer for drop-audio replacement and automap observations |

A foreign provider is accepted only when Diagnostics proves the relevant
D2RLoader-tracked owner and that same provider publishes the matching service.
Unknown, untracked, mismatched, or unavailable providers still fail closed
without disabling unrelated UnHoarder features.

See [docs/native-hook-interop.md](docs/native-hook-interop.md) for the drop/automap service, borrowed pointer lifetime and remaining collision boundaries.

See [docs/tooltip-compat.md](docs/tooltip-compat.md) for the developer contract,
lifetime rules, provider requirements, and load-order behavior. Public ABI
headers live under [interop/](interop/).
