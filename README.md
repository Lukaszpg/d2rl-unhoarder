# UnHoarder

A loot filter for Diablo II: Resurrected running through D2RLoader.

**Original author: MindH1ve.** This is Pyre-Flies' fork of
[Lukaszpg/d2rl-unhoarder](https://github.com/Lukaszpg/d2rl-unhoarder), carrying
controller, sound and compatibility fixes for our Reimagined testing. The
original author credits, plugin identity and GPL-3.0 license are retained.

## What changed in this fork

**1.2.19** includes quest weapons and jewelry (such as Wirt's Leg and Amulet of the Viper) in `"ques"` / `"Quest"` `itemType` rules, logs a warning instead of rejecting `filter.json` when a rule references an unknown or zero-item `itemType`, fixes minimap icons on controller ground labels, and includes the 1.2.16–1.2.17 improvements.
Both plugins work independently;
use **Controller QOL 1.3.1+rev.93** or newer when combining them. Both perf commands remain
available. Live loot performance improved in user testing; complete ALT,
controller and hosted interaction coverage remains open.

- Apply filter names, colors and backgrounds to controller ground labels.
- Play selected filter alerts when items drop, including when labels are hidden.
  Successful filter playback replaces the ordinary drop sound; unavailable or
  rejected playback falls back to the ordinary sound.
- Route the bundled filter alerts through the game's Items volume control.
- Restore socket-based rules with the specifically reviewed Maps companion.
- Block keyboard/mouse pickup of items hidden by a filter rule, including clicks
  on their ground gear icon. Hold either keyboard ALT key to allow pickup.
  Controller pickup retains its existing policy. Share confirmed
  visibility with matching Controller QOL so hidden loot does not consume LB
  pickup slots; QOL also applies this to unmodified controller pickup when
  `block_filtered_pickup` is enabled.

Download **1.2.19** from this fork's
[latest release](https://github.com/Pyre-Flies/d2rl-unhoarder/releases/latest).
It includes the earlier background, controller-label and sound fixes.
Upstream release assets do not include this fork's changes.

## Install or update

With the game closed, back up your current files and put `unhoarder.dll` in:

`<D2R_installation_directory>\mods\<your_mod_name>\d2rloader\plugins`

Keep your existing `filter.json` in:

`<D2R_installation_directory>\mods\<your_mod_name>\d2rloader\config`

Use [UnHoarder Builder](https://lukaszpg.github.io/unhoarder-builder/) to create
or edit your rules. Saving a valid filter reloads it automatically;
**Ctrl+Shift+F9** also reloads it. Invalid JSON leaves the last valid rules active.
Updates should preserve your filter and other saved settings.

### Filter sounds

The DLL alone does not install the alerts. The matching data payload belongs
inside the selected mod's MPQ/data tree and includes the 16 `Filter01` through
`Filter16` audio files. If the mod already supplies `sounds.txt`, merge those
rows into its existing table instead of replacing the table with the generic
one. Preserve that mod's row indexes and unrelated values.

For those 16 rows, set `Channel` to `sfx/items_hd` and `IsUI` to `0`. Keep the
filenames, volume, pitch and other fields unchanged. A launcher that restores
its own clean table must preserve these changes during package preparation.
See [packaging notes](docs/PACKAGING.md).

## Compatibility and testing

Requires Windows x64 and the reviewed D2RLoader ABI-4 environment. The inherited
native profile is qualified for D2R build 93847; individual companion paths are
admitted only when their exact compatibility checks pass. A version label alone
is not enough. Unsupported native paths remain disabled with a diagnostic.

The release passes **50 CTest tests and 51 Python source checks**, with 79
matching Controller QOL tests. Live testing shows improved loot performance.
Full ALT/controller interaction, audio and broader hosted coverage remain open.
Automated checks do not establish full Ladder qualification.

See the [changelog](CHANGELOG.md) for the player-facing changes. Build-specific
records live under [docs/](docs/) for contributors.

## Identity

- product: **UnHoarder**
- CMake target: `unhoarder`
- DLL: `unhoarder.dll`
- D2RLoader plugin id: `unhoarder`
- log: `unhoarder.log`
- config: `d2rloader/config/filter.json`

The former binary name `loot-filter.dll` is retired. UnHoarder refuses to start if that old production DLL is already loaded, preventing two production versions from competing for the same native hooks.

The runtime configuration is `<mod>/d2rloader/config/filter.json`. UnHoarder no longer reads filter JSON files from the `plugins` directory.

## Performance reports

Run `/unhoarder perf` before and after a 30-60 second test. Each report covers
the interval since the preceding report and writes to `unhoarder.log`. It does
not change settings. Pair it with an FPS overlay; nested timings are not additive.

Build instructions and integration details are in [development](DEVELOPMENT.md).

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

`code`, `baseName`, `itemType`, `quantity`, `rarity`, `itemLevel`, `sockets`, `ethereal`, `identified`, and `sellPrice` are currently supported. Numeric conditions accept `eq`, `gt`, `gte`, `lt`, and `lte`.

`sellPrice` is the native vendor sell value in gold. It is evaluated lazily and kept in a short-lived ground-item cache; pickup/inventory observation invalidates the entry immediately. Inferior/normal/superior and magic/rare items are supported. For **unidentified magic/rare**, D2R's native transaction calculator excludes affix and bonus-stat price contributions, so `sellPrice` is the visible/base-state value only. After identification, the same condition sees the full affix-aware native sell value.

The filter intentionally does not expose hidden unidentified affixes or unique identity. An unidentified magic/rare `sellPrice` is therefore a conservative lower bound: a high base-value item stays high-value, while an item below a threshold may rise above it after identification.

## Runtime behavior

A valid JSON file activates the filter automatically. Saving the JSON triggers an atomic live reload after the file is stable; **Ctrl+Shift+F9** remains as the manual reload shortcut. Invalid JSON keeps the previous valid ruleset active. If startup begins with a missing or invalid filter, correcting the file in the same game session activates the first valid ruleset automatically; no restart is required.

Production functionality includes ground label text/background styling, custom names, stack quantity display, drop sounds, Show/Hide visibility, qualified pickup suppression for hidden items, and JSON-driven automap icons using D2R's native automap projection. These core features run through UnHoarder's own qualified D2RLoader-managed native hooks and have no mod-specific callback/export dependency.

Operational logs are limited to configuration, compatibility/readiness, reloads, and actionable failures.

## Examples

Use `unhoarder.v3.example.json` as the canonical starting point. Additional schema-3 examples cover Continue, base names, item types, sockets, ethereal, identified, and native `sellPrice` conditions.
