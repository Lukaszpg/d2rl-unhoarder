# Packaging UnHoarder for a mod

Ship `unhoarder.dll` in the selected mod's `d2rloader/plugins/` folder. Preserve
existing `d2rloader/config/filter.json` and all other saved settings.

The sound data belongs inside that mod's MPQ/data payload, preserving the
`data/hd/global/sfx/item/Filter01.flac` through `Filter16.flac` paths. If the mod
already supplies `data/global/excel/sounds.txt`, merge the filter rows into that
table. Do not replace a mod's table with this repository's generic table.

For Filter01..Filter16, use `Channel=sfx/items_hd` and `IsUI=0`. Preserve the
mod's indexes, filenames, volume, pitch and unrelated fields. The team staging
ZIP contains a Reimagined-specific table; review it against the ladder's current
table before integration. That full mod table is not part of this source commit.

Launchers may restore their clean source table before applying tweaks. Update
that clean source or a supported post-restore override so the filter routing
survives package preparation. A DLL-only update cannot install these table edits.

Keep author credits, GPL-3.0 and dependency notices with redistribution, and
provide the corresponding source. Version 1.2.19 is distributed as a Reimagined
upgrade ZIP and corresponding-source archive. Its exact DLL is paired with
Controller QOL rev.93+; rebuilding the same version does not automatically
establish that companion's artifact compatibility.
