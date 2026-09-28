# D2RL Loot Filter

**D2RL Loot Filter** is a native, JSON-configurable loot filter for **Diablo II: Resurrected**.

It provides ordered `Show` / `Hide` rules, ground-label styling, custom names, sounds, stack-aware conditions, pickup suppression for hidden items, and configurable automap markers — all driven by a live-reloadable `filter.json`.

> **Version:** 1.0.0  
> **Author:** MindH1ve  
> **Plugin ID:** `loot-filter`  
> **DLL:** `loot-filter.dll`  
> **Configuration:** `filter.json`

## Features

- PoE-style ordered **Show / Hide** rule blocks.
- `Continue` support for composing multiple matching rules.
- Match items by:
  - item code;
  - weapon/armor base name;
  - item type;
  - quantity;
  - rarity;
  - item level;
  - socket count;
  - ethereal state;
  - identified state.
- Replace the displayed ground-item name.
- Change ground-tooltip text color.
- Change ground-tooltip background color.
- Play a named D2R `sounds.txt` sound for matching ground items.
- Draw configurable automap icons with:
  - circle, diamond, triangle, or star shapes;
  - independent border and fill colors;
  - configurable size.
- Hide unwanted ground-item labels.
- Suppress the qualified ground-item pickup path for hidden items.
- Display stack quantities using the game's ground-label path.
- Automatic live reload when `filter.json` changes.
- Manual reload with **Ctrl+Shift+F9**.
- Atomic configuration reload: an invalid edit does not replace the last valid ruleset.
- Fail-open handling for unavailable native item properties so an unresolved condition does not accidentally hide an item.

The filter intentionally does **not** expose hidden affixes or hidden unique identity. It is designed not to reveal information that the player could not legitimately infer from the item on the ground.

---

## Requirements

Loot Filter 1.0.0 currently targets:

- **Diablo II: Resurrected build 93847**
- **D2RLoader 1.3.x / PluginSDK v4**
- a **mod-scoped** D2RLoader installation

---

## Installation

Place the plugin and configuration in the mod-scoped D2RLoader `plugins` directory:

```text
<Diablo II Resurrected>/
└── mods/
    └── <your-mod>/
        └── d2rloader/
            └── plugins/
                ├── loot-filter.dll
                └── filter.json
```

`filter.json` must be beside `loot-filter.dll`.

New installations should always use `filter.json`.

---

## Quick start

Create `filter.json` beside the DLL:

```json
{
  "version": 3,
  "rules": [
    {
      "show": {
        "conditions": {
          "code": "divo"
        },
        "tooltip": {
          "backgroundColor": "RGBA(110, 35, 160, 0.82)",
          "textColor": "RGBA(180, 140, 255, 1)"
        },
        "dropSound": "Drop_Zing",
        "minimapIcon": {
          "shape": "diamond",
          "borderColor": "RGBA(225, 205, 255, 1)",
          "fillColor": "RGBA(180, 140, 255, 0.82)",
          "size": 24
        }
      }
    },
    {
      "hide": {
        "conditions": {
          "rarity": "normal"
        }
      }
    }
  ]
}
```

Save the file while the game is running. Loot Filter watches the configuration and reloads it automatically after the file has remained stable briefly.

You can also force a reload with:

```text
Ctrl + Shift + F9
```

---

## Rule structure

Schema 3 uses an ordered block model inspired by Path of Exile item filters.

Every element of `rules` must contain **exactly one** `show` or `hide` object:

```json
{
  "version": 3,
  "rules": [
    {
      "show": {
        "conditions": {
          "rarity": "unique"
        }
      }
    },
    {
      "hide": {
        "conditions": {
          "rarity": "normal"
        }
      }
    }
  ]
}
```

Rules are evaluated from top to bottom.

A matching block normally applies its visibility/actions and stops evaluation. Add:

```json
"continue": true
```

to apply that block and continue evaluating later rules.

### Catch-all rules

`conditions` may be omitted in schema 3. This makes the block match everything that reaches it.

For example, a strict whitelist can end with:

```json
{
  "hide": {}
}
```

---

## Conditions

Conditions inside one block are combined with **AND**.

Arrays inside a condition are combined with **OR**.

For example:

```json
{
  "show": {
    "conditions": {
      "itemType": ["swor", "axe"],
      "rarity": ["rare", "unique"],
      "itemLevel": {
        "gte": 85
      }
    }
  }
}
```

means:

```text
(sword OR axe)
AND
(rare OR unique)
AND
(item level >= 85)
```

### `code`

Matches the item's internal item code.

```json
"code": "divo"
```

or:

```json
"code": ["divo", "exo", "bst"]
```

Item codes are 1-4 printable ASCII characters.

### `baseName`

Matches an exact weapon or armor base name from the active mod data.

```json
"baseName": "Sacred Armor"
```

or:

```json
"baseName": ["Sacred Armor", "Archon Plate"]
```

`baseName` is resolved from `weapons.txt` and `armor.txt` when the filter is loaded.

### `itemType`

Matches an `ItemTypes.Code` or literal `ItemTypes.ItemType` name from the active mod data.

```json
"itemType": "swor"
```

or:

```json
"itemType": ["swor", "axe"]
```

Item-type equivalences are expanded from the active `itemtypes.txt` hierarchy when the rules are loaded.

### `rarity`

Supported rarity names are:

```text
inferior
normal
superior
magic
set
rare
unique
crafted
tempered
```

Example:

```json
"rarity": ["rare", "unique"]
```

The filter deliberately matches **rarity**, not hidden unique-item identity.

### `quantity`

Matches stack quantity.

```json
"quantity": {
  "gte": 10
}
```

Valid range: `0..65535`.

### `itemLevel`

Matches item level.

```json
"itemLevel": {
  "gte": 85
}
```

Valid range: `1..99`.

### `sockets`

Matches the number of sockets on the item.

```json
"sockets": {
  "gte": 4
}
```

Valid range: `0..15`.

### `ethereal`

Matches ethereal state.

```json
"ethereal": true
```

or:

```json
"ethereal": false
```

### `identified`

Matches identified state.

```json
"identified": true
```

or:

```json
"identified": false
```

---

## Numeric comparisons

`quantity`, `itemLevel`, and `sockets` use comparison objects.

Supported operators are:

| Operator | Meaning |
|---|---|
| `eq` | equal to |
| `gt` | greater than |
| `gte` | greater than or equal to |
| `lt` | less than |
| `lte` | less than or equal to |

Example range:

```json
"itemLevel": {
  "gte": 85,
  "lte": 90
}
```

Only one lower bound (`gt` or `gte`) and one upper bound (`lt` or `lte`) may be used in the same comparison object.

---

## Actions

A `show` or `hide` block may contain actions in addition to visibility.

Supported schema-3 fields are:

```text
conditions
continue
name
tooltip
dropSound
minimapIcon
```

### Custom ground-item name

Use `name` to replace the displayed ground label:

```json
{
  "show": {
    "conditions": {
      "code": "divo"
    },
    "name": "DIVINE ORB"
  }
}
```

Names support printable ASCII text with these limits:

- maximum 79 bytes;
- maximum 3 non-empty lines;
- maximum 55 characters per line.

A multiline name may use JSON newline escapes:

```json
"name": "IMPORTANT\nDIVINE ORB"
```

### Tooltip colors

Schema 3 keeps tooltip colors inside the `tooltip` object:

```json
"tooltip": {
  "backgroundColor": "RGBA(110, 35, 160, 0.82)",
  "textColor": "RGBA(180, 140, 255, 1)"
}
```

Either color can be supplied independently.

Color format:

```text
RGBA(red, green, blue, alpha)
```

where:

- red: `0..255`
- green: `0..255`
- blue: `0..255`
- alpha: `0..1`

Example:

```json
"textColor": "RGBA(130, 197, 255, 1)"
```

Flat `backgroundColor` / `textColor` fields from older schema versions are not valid in schema 3.

### Drop sound

Use `dropSound` with a D2R `sounds.txt` **row name**:

```json
"dropSound": "Drop_Zing"
```

The value is a sound row name, not a numeric sound index or an FMOD filename.

Valid names are 1-63 ASCII characters containing letters, numbers, `_`, or `-`.

In 1.0.0 the sound is triggered by the plugin's qualified first-observed ground-item path rather than a native item-drop event.

### Automap icon

Example:

```json
"minimapIcon": {
  "shape": "diamond",
  "borderColor": "RGBA(225, 205, 255, 1)",
  "fillColor": "RGBA(180, 140, 255, 0.82)",
  "size": 24
}
```

Supported shapes:

```text
circle
diamond
triangle
star
```

`borderColor`, `fillColor`, and `shape` are required.

`size` is optional:

- default: `12`
- minimum: `12`
- maximum: `40`
- values outside the range are clamped to `12..40`

Markers use D2R's native automap projection and are shown only while the automap is active.

---

## `Continue`

`continue` allows several rules to contribute to the final result.

Example:

```json
{
  "version": 3,
  "rules": [
    {
      "show": {
        "conditions": {
          "itemType": "Currency"
        },
        "tooltip": {
          "textColor": "RGBA(220, 220, 220, 1)"
        },
        "continue": true
      }
    },
    {
      "show": {
        "conditions": {
          "code": "divo"
        },
        "tooltip": {
          "backgroundColor": "RGBA(110, 35, 160, 0.82)"
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
      "hide": {}
    }
  ]
}
```

If a Divine Orb matches both blocks, it inherits the text color from the first block and the background, sound, and minimap icon from the second block.

When several continued rules set the same property, the later matching rule wins for that property. Actions not replaced by a later rule remain active.

> **1.0.0 limitation:** there is currently no explicit `null` / reset action for removing an action inherited from an earlier continued block.

---

## More examples

### Show high-item-level rare bows

```json
{
  "show": {
    "conditions": {
      "code": "lbw",
      "rarity": "rare",
      "itemLevel": {
        "gte": 85
      }
    },
    "tooltip": {
      "textColor": "RGBA(180, 140, 255, 1)"
    }
  }
}
```

### Highlight ethereal 4+ socket body armor

```json
{
  "show": {
    "conditions": {
      "itemType": "tors",
      "ethereal": true,
      "sockets": {
        "gte": 4
      }
    },
    "tooltip": {
      "backgroundColor": "RGBA(130, 197, 255, 0.82)"
    }
  }
}
```

### Hide low-rarity versions of one base

```json
{
  "hide": {
    "conditions": {
      "baseName": "Sacred Armor",
      "rarity": ["inferior", "normal", "superior"]
    }
  }
}
```

### Show unidentified shields

```json
{
  "show": {
    "conditions": {
      "itemType": "shld",
      "identified": false
    },
    "tooltip": {
      "textColor": "RGBA(130, 197, 255, 1)"
    }
  }
}
```

### Whitelist with a final Hide rule

```json
{
  "version": 3,
  "rules": [
    {
      "show": {
        "conditions": {
          "rarity": ["unique", "set"]
        }
      }
    },
    {
      "show": {
        "conditions": {
          "code": ["divo", "exo"]
        }
      }
    },
    {
      "hide": {}
    }
  ]
}
```

---

## Live reload and error handling

Loot Filter watches `filter.json` while the game is running.

When the file changes:

1. the plugin waits for the file to become stable;
2. it parses and validates the complete configuration;
3. it builds a new immutable ruleset;
4. only a fully valid ruleset replaces the active one.

If an edit is invalid, the previous valid configuration remains active.

The same atomic reload behavior applies to the active Excel tables used by `baseName` and `itemType` resolution.

Current limits:

```text
Maximum filter file size: 64 KiB
Maximum rules:            256
```

---

## Fail-open behavior

Conditions such as rarity, item level, socket count, ethereal state, and identified state depend on qualified native item properties.

If a rule could match but a required native value is unavailable, Loot Filter does **not** treat the missing value as `0` or `false`. Evaluation stops at that unresolved point and the item remains visible rather than allowing a later broad `hide` rule to conceal it accidentally.

This behavior is intentional.

---

## What `Hide` does

`hide` removes the item's ground label and participates in the plugin's qualified hidden-item pickup suppression.

It does **not** currently remove the item's 3D world model.

Pickup suppression is tied to the qualified ground-item interaction path and should not be interpreted as a universal engine-level deletion of the item.

---

## Automap compatibility

Loot Filter 1.0.0 contains its own standalone D3D12/ImGui automap-marker renderer and does not depend on MapSense or Floating Damage.

MapSense currently uses the same native automap projection rendezvous. To avoid blindly chaining conflicting hooks, Loot Filter fails closed for its minimap projection when MapSense is already loaded.

The rest of the loot filter continues to operate; only Loot Filter's automap-marker functionality is unavailable in that configuration.

---

## Design principles

Loot Filter follows a few deliberate rules:

- filtering should be based on information legitimately available from the dropped item;
- unidentified items should not reveal hidden affixes;
- multi-unique bases should not reveal which unique rolled before identification;
- unknown native data must never become an accidental hide condition;
- invalid live edits must never destroy the last working filter;
- rule ordering should remain predictable and readable.

---

## Configuration reference

A complete schema-3 block can look like this:

```json
{
  "show": {
    "conditions": {
      "code": ["divo", "exo"],
      "baseName": ["Sacred Armor"],
      "itemType": ["tors"],
      "quantity": {
        "gte": 1
      },
      "rarity": ["rare", "unique"],
      "itemLevel": {
        "gte": 85,
        "lte": 99
      },
      "sockets": {
        "gte": 4
      },
      "ethereal": true,
      "identified": false
    },
    "continue": true,
    "name": "IMPORTANT ITEM",
    "tooltip": {
      "backgroundColor": "RGBA(110, 35, 160, 0.82)",
      "textColor": "RGBA(180, 140, 255, 1)"
    },
    "dropSound": "Drop_Zing",
    "minimapIcon": {
      "shape": "diamond",
      "borderColor": "RGBA(225, 205, 255, 1)",
      "fillColor": "RGBA(180, 140, 255, 0.82)",
      "size": 24
    }
  }
}
```

This example demonstrates syntax only; because conditions are ANDed, a real rule should include only the conditions relevant to the intended item set.

---

## Legacy schemas

The runtime still accepts schema versions `1` and `2` as migration formats.

New filters should use:

```json
"version": 3
```

Schema 3 is the canonical format and is the only format documented here.

---

## Building from source

When source is available, the plugin is built as part of the RuffnecKk D2RLoader Suite tree.

Expected source location:

```text
<suite>/plugins/loot-filter/
```

The CMake target is:

```text
loot_filter
```

The resulting binary is:

```text
loot-filter.dll
```

The project currently uses C++20 and links against D2RLoader PluginSDK v4, MinHook, Dear ImGui, DXGI, and nlohmann/json through the Suite build.

---

## Troubleshooting

**The filter does not load**  
Make sure `filter.json` is beside `loot-filter.dll` and contains a valid top-level object with `"version": 3` and a `rules` array.

**My changes do not appear**  
Wait briefly for automatic reload or press **Ctrl+Shift+F9**.

**An invalid edit broke my filter**  
It should not. Loot Filter keeps the previous valid ruleset active when a reload fails. Check the D2RLoader log for a `LOOT_RULES_REFUSED` message.

**A `baseName` rule is rejected**  
The name must match an exact entry from the active `weapons.txt` or `armor.txt` data.

**An `itemType` rule is rejected**  
Use a valid `ItemTypes.Code` or `ItemTypes.ItemType` from the active `itemtypes.txt` data.

**My drop sound does not play**  
`dropSound` expects a `sounds.txt` row name, not a numeric sound ID or audio filename.

**Minimap icons do not appear while MapSense is loaded**  
This is expected in 1.0.0. The minimap projection hook deliberately refuses to install when MapSense owns the same native rendezvous.

---

## License

See the repository's `LICENSE` file for the terms applying to Loot Filter and `THIRD_PARTY_NOTICES` for licenses and attribution relating to third-party components.

---

## Disclaimer

Loot Filter is an unofficial third-party modification for Diablo II: Resurrected. It is not affiliated with or endorsed by Blizzard Entertainment.
