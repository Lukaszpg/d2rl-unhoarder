# UnHoarder

**Author:** MindH1ve

Standalone D2RLoader plugin providing a production JSON loot filter for Diablo II: Resurrected build 93847 / Sanctuary of Exile.

## Identity

- product: **UnHoarder**
- CMake target: `unhoarder`
- DLL: `unhoarder.dll`
- D2RLoader plugin id: `loot-filter` *(kept for runtime/interoperability compatibility)*
- config: `filter.json`

The former binary name `loot-filter.dll` is retired. UnHoarder refuses to start if that old production DLL (or the older probe DLL) is already loaded, preventing two versions from competing for the same native hooks.

The canonical runtime configuration is `filter.json`. If it is absent, the plugin can still read `loot-filter.json` (v0.2.76) or the older `loot-filter-probe.json` beside the DLL as migration fallbacks. New configurations should use `filter.json`.

## Standalone build

UnHoarder no longer needs to live under `RuffnecKk-D2RLoader-Suite/plugins/` and does not configure the Suite root.

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

Place `filter.json` beside the DLL.

### PluginSDK source

By default CMake fetches the public D2RLoader PluginSDK directly from `D2RLoader/PluginSDK`, pinned to commit `717f727a0ec52912d1558764345f8fa3453a2bd6` (SDK 0.3.0 / plugin ABI 4) for reproducible builds. No RuffnecKk Suite source checkout is used by the build.

For an offline/local SDK checkout, configure with:

```powershell
cmake -S . -B build `
  -G "Visual Studio 17 2022" -A x64 `
  -DUNHOARDER_PLUGINSDK_SOURCE_DIR="D:/path/to/PluginSDK"
```

## GitHub Actions

`VERSION` is the canonical product version. `.github/workflows/ci.yml` validates pushes and pull requests with Python tests, portable C++ tests, and a Release DLL build. `.github/workflows/release.yml` is manually dispatched with a major/minor/patch bump; it updates `VERSION`, repeats the validation gates, commits and tags the release, and publishes `unhoarder.dll`, a source ZIP, and SHA-256 checksums to GitHub Releases.

## Canonical JSON structure

Schema 3 mirrors Path of Exile's ordered Show/Hide block model. Every element of `rules` contains exactly one `show` or `hide` wrapper. Matching normally stops at that block; `"continue": true` applies the block and continues to later rules, allowing actions to compose and later values to override earlier ones.

```json
{
  "version": 3,
  "rules": [
    {
      "show": {
        "ruleName": "High-value currency",
        "conditions": { "code": "divo" },
        "tooltip": {
          "backgroundColor": "RGBA(110, 35, 160, 0.82)",
          "textColor": "RGBA(180, 140, 255, 1)"
        },
        "dropSound": "Drop_Zing",
        "minimapIcon": {
          "shape": "diamond",
          "borderColor": "RGBA(225, 205, 255, 1)",
          "fillColor": "RGBA(180, 140, 255, 0.82)",
          "size": 20
        }
      }
    },
    {
      "hide": {
        "conditions": { "rarity": "normal" }
      }
    }
  ]
}
```

`ruleName` is optional user-facing metadata for tools such as **UnHoarder - Builder**. UnHoarder accepts it as schema metadata but deliberately ignores it during runtime rule matching and action evaluation; the plugin does not rewrite the filter file.

`minimapIcon.size` is clamped to **12..40 px**, with **12 px** as the default. Supported shapes are `circle`, `diamond`, `triangle`, and `star`.

## Supported conditions

`code`, `baseName`, `itemType`, `quantity`, `rarity`, `itemLevel`, `sockets`, `ethereal`, and `identified` are currently supported. Numeric conditions accept `eq`, `gt`, `gte`, `lt`, and `lte`.

The filter intentionally does not expose hidden unidentified affixes or unique identity. It should not reveal information the player cannot legitimately infer from the dropped item.

## Runtime behavior

A valid JSON file activates the filter automatically. Saving the JSON triggers an atomic live reload after the file is stable; **Ctrl+Shift+F9** remains as the manual reload shortcut. Invalid JSON keeps the previous valid ruleset active.

Production functionality includes ground label text/background styling, custom names, stack quantity display, drop sounds, Show/Hide visibility, qualified pickup suppression for hidden items, and JSON-driven automap icons using D2R's native automap projection.

Legacy reverse-engineering capture hotkeys and startup probe dumps are not part of the production runtime anymore. Operational logs are limited to configuration, compatibility/readiness, reloads, and actionable failures.

## Examples

Use `loot-filter.v3.example.json` as the canonical starting point. Additional schema-3 examples cover Continue, base names, item types, sockets, ethereal and identified conditions.
