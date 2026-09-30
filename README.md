# UnHoarder

**Author:** MindH1ve

Standalone D2RLoader plugin providing a production JSON loot filter for Diablo II: Resurrected build 93847.

## Installation

1. Head to releases and download the newest release - `unhoarder.dll` and `unhoarder-<version>-data.zip` are required.
2. Place `unhoarder.dll` in `<D2R_installation_directory>\mods\<your_mod_name>\d2rloader\plugins`
3. Unzip the downloaded ZIP, but do not copy it yet to your D2R directory.
4. If you are:
   
  a) NOT Using a mod that modifies `sounds.txt` file - you can copy the unzipped contents to `<D2R_installation_directory>\mods\<your_mod_name>\<your_mod_name>.mpq\`

  b) If you are using a mod that modifies `sounds.txt` file, you have to manually add the filter alert sounds to that file, using for example [TXTEditor](https://github.com/yinyin333333/TXTeditor):
  - Open the downloaded `sounds.txt` file
  - Open the `sounds.txt` file of the mod you're using
  - Copy `Filter01` to `Filter16` rows and add them to your mods `sounds.txt` at the end of the file
  - Make sure to change the IDS in `Index` column
  - Save the file

## Building and installing a filter

In order to build a filter, head to [UnHoarder Builder](https://lukaszpg.github.io/unhoarder-builder/) site. Upload the required text files of the mod you're using (or vanilla if you're not using any huge overhaul mods) to enable autocomplete. Build your filter and then click `Download filter.json` at the top.

The `filter.json` file has to be placed in the same `plugins` folder as `unhoarder.dll`.

## Identity

- product: **UnHoarder**
- CMake target: `unhoarder`
- DLL: `unhoarder.dll`
- D2RLoader plugin id: `loot-filter` *(kept for runtime/interoperability compatibility)*
- config: `filter.json`

The former binary name `loot-filter.dll` is retired. UnHoarder refuses to start if that old production DLL (or the older probe DLL) is already loaded, preventing two versions from competing for the same native hooks.

The canonical runtime configuration is `filter.json`. If it is absent, the plugin can still read `loot-filter.json` (v0.2.76) or the older `loot-filter-probe.json` beside the DLL as migration fallbacks. New configurations should use `filter.json`.

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

Place `filter.json` beside the DLL.

### PluginSDK source

By default CMake fetches the public D2RLoader PluginSDK directly from `D2RLoader/PluginSDK`, pinned to commit `717f727a0ec52912d1558764345f8fa3453a2bd6` (SDK 0.3.0 / plugin ABI 4) for reproducible builds.

For an offline/local SDK checkout, configure with:

```powershell
cmake -S . -B build `
  -G "Visual Studio 17 2022" -A x64 `
  -DUNHOARDER_PLUGINSDK_SOURCE_DIR="D:/path/to/PluginSDK"
```

## Plugin compatibility

Plugins that share UnHoarder's native ground-tooltip render hooks can opt into
the **UnHoarder Tooltip Compatibility API v1** instead of installing a second
hook on the same entry point. The contract uses D2RLoader
`DiagnosticsService` for owner discovery and `PluginCommunicationService`
for a versioned middleware table; there is no DLL import or foreign detour
decoding.

V1 intentionally covers only the shared label painter (`0x1FA8E0`) and glyph
renderer (`0x658510`). Other native-hook collisions retain UnHoarder's
fail-closed behavior.

See [docs/tooltip-compat.md](docs/tooltip-compat.md) and copy
[interop/unhoarder_tooltip_compat_v1.hpp](interop/unhoarder_tooltip_compat_v1.hpp)
into a compatible plugin.

## Canonical JSON structure

Schema 3 mirrors Path of Exile's ordered Show/Hide block model. Every element of `rules` contains exactly one `show` or `hide` wrapper. Matching normally stops at that block; `"continue": true` applies the block and continues to later rules, allowing actions to compose and later values to override earlier ones.

```json
{
  "version": 3,
  "rules": [
    {
      "show": {
        "ruleName": "High-value currency",
        "conditions": { "code": "r33" },
        "tooltip": {
          "backgroundColor": "RGBA(110, 35, 160, 0.82)",
          "textColor": "RGBA(180, 140, 255, 1)"
        },
        "dropSound": "Filter06",
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

Production functionality includes ground label text/background styling, custom names, stack quantity display, drop sounds, Show/Hide visibility, qualified pickup suppression for hidden items, and JSON-driven automap icons using D2R's native automap projection. These core features run through UnHoarder's own qualified D2RLoader-managed native hooks and have no mod-specific callback/export dependency.

Legacy reverse-engineering capture hotkeys and startup probe dumps are not part of the production runtime anymore. Operational logs are limited to configuration, compatibility/readiness, reloads, and actionable failures.

## Examples

Use `loot-filter.v3.example.json` as the canonical starting point. Additional schema-3 examples cover Continue, base names, item types, sockets, ethereal and identified conditions.
