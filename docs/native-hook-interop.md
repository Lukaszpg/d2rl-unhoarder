# Cooperative native-item hook interoperability (ABI v1)

This branch imports Pyre-Flies UnHoarder 1.2.19 and adds an opt-in,
D2RLoader-managed interoperability contract for **two native hook entries**:

| Verified D2R entry RVA | Role | If owned by UnHoarder | If owned by another plugin |
| --- | --- | --- | --- |
| `0x1C8130` | Native item drop-audio | synchronous replacement subscribers, filtered sound has first refusal, then native default | accept only diagnosed tracked single owner publishing compatible service |
| `0xD76E0` | Native automap unit render | synchronous post-native observers (borrowed pointers), then own projection | accept only diagnosed tracked single owner publishing compatible service |

The header `interop/native_item_hook_compat_v1.hpp` defines
`d2rl-native-item-hook-compat` version 1. It exposes four typed
register/unregister callbacks and exact RVA/size/version validation.
Potential foreign providers must implement this exact ABI themselves;
**merely having another native hook does not make a plugin compatible**.

### Ownership and safety

- Consumers use D2RLoader DiagnosticsService to identify a **tracked,
  unique, call-through** native owner and verify range ownership, then
  acquire the matching provider ID and exact service with
  PluginCommunicationService. DLL names and loading order alone cannot
  authorize compatibility.
- Callback pointers and game-owned item/automap pointers are borrowed for
  synchronous dispatch only. The provider never retains those native pointers.
- No direct hook chaining, arbitrary trampoline reuse, manual code patches,
  or rescanning to guess a relocated target is permitted.
- The bounded subscriber registry serializes registration and dispatch
  snapshots, has a constant-time no-listeners fast path, and makes a successful
  unregister a quiescence barrier. Do not unregister from within the same
  callback; plugin unload must unregister while its code is still mapped.
- Drop-audio callbacks return `true` only when they actually accepted
  replacement playback. At most one replacement wins. Otherwise the host
  invokes the original native drop sound exactly once.
- Automap observer callbacks execute **after** the host forwards the native
  renderer. Observers must not write to game memory, retain borrowed
  pointers or modify rendering state.
- An unknown, untracked, multiply owned or uncooperative native entry
  disables only the contended feature. Unhoarder will not overwrite it.
- Existing tooltip paint/glyph services, stat-reader service and in-world
  label service remain unchanged; the pickup dispatcher and item-code reader
  still have no cooperative hook API and must refuse collision.

### Cache and asset hardening

All negative automap cache reads/writes now use the same mutex as session
reset and style invalidation; a contended advisory cache operation is skipped.
The bundled `Filter01` through `Filter16` asset precheck falls back to
the original native drop sound when the asset is missing **or the mod data
directory cannot be established**. MPQ-only assets that cannot be proven
by loose-file discovery intentionally fall back. Other arbitrary sound names
have no guaranteed audibility detection.

### Qualification and release gate

Builds retain exact 93847 native byte, callsite, PE and ABI checks; loosening
the reported build-version string is **not** multi-build proof. Before
merging, run CI on this PR and retest both plugin load orders, service
unregister/unload, unknown foreign hooks, mouse/controller pickup and ALT,
map overlays, UI sound volume, missing audio, and repeated Save & Exit.
The import baseline documents incomplete live test coverage; no source test
asserts that runtime acceptance is complete.
