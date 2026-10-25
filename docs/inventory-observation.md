# Inventory observation and the 1.2.10 candidate

The pickup observer uses the public InventoryService on the UI thread. It
reads copied inventory/cursor identities; it never writes an item, changes
storage or keeps a native pointer. The timer interval remains 150 ms and a
pending token still prevents overlapping queued snapshots.

## Feature demand

`ReadPickupObservationNeeds` in [plugin.cpp](../src/plugin.cpp) takes one
immutable rules snapshot. With no rules or inactive native filtering it
returns no demand. Otherwise it enables sound observation only while sound
is armed, icon observation only with icon rules and admitted native projection,
and price observation only with sell-price rules. Installed projection by
itself no longer causes inventory enumeration.

Both the worker and the queued UI callback check current demand. A queued
callback after a reload can therefore stop without enumerating, and still
releases its pending token. Active snapshots retain Inventory, Equipment,
Belt, Cube, PersonalStash, SharedStash and CustomPage coverage, plus the
separately qualified cursor observation. Each snapshot invalidates only the
caches of its active features. Ground and incomplete copied records are ignored.

Sound selection/playback, native drop-event forwarding/fallback, same-ID
re-drops, minimap projection/renderer setup, visibility and property admission
are unchanged. The legacy sound observation remains at its existing cadence
while armed. The JSON watcher is unchanged; no new settings are introduced.

## Vendor-price cache

Lookup and insertion already use `unitId % 512`. Invalidation now examines
that same slot while holding the same mutex and still requires an exact ID
match. A colliding item's cache entry is retained. Native calls remain outside
the cache lock; unknown values, pointer/class/code mismatches, time reversal
and the existing 1,000 ms TTL retain their earlier failure behavior.

The cache key also contains the rules generation supplied by the caller's
immutable filter snapshot. This matters when price observation is paused:
an item may change while price rules are disabled and those rules may return
before the old TTL expires. A new generation cannot consume that old cached
price, including when an older in-flight reader stores its result later.
The old snapshot can still consume its own correctly keyed value.

## Exact QOL companion admission

The 1.2.10 candidate's SHA256 is
`3e5f3ed2c5819035ab376dff60b1597a70a0710447370aa30dab3e5aa2262e88`.
Its image size is `0xE7000`. The Release link map identifies the unchanged
one-argument item-code forwarding wrapper at DLL RVA `0x1EA40`, with original
function-pointer slot `0xB47B8`. Its 19 bytes are:

```text
48 83 EC 28 48 8B 05 6D 5D 09 00 FF D0 90 48 83 C4 28 C3
```

The RIP-relative load resolves to that slot; the wrapper calls the original
once and preserves its argument and return. Matching QOL rev.83 admits this
exact disk hash with the complete existing live forwarding/page/ownership
proof. A version number or arbitrary rebuild is not admission evidence.
No D2R/native entry, guard bytes, provider ABI or native data offsets changed.

## Performance reports

Enter `unhoarder perf` in D2RLoader's console, or `/unhoarder perf` through
the chat command route. A bounded `LOOT_PERF` report also goes to the plugin
log. It changes no filter setting and schedules no observation or native work.

The state line identifies loaded/active rules and current sound/icon/price
polling demand. The counters separate worker wakeups, inactive/service skips,
already-pending work, dispatch failures, actual polls and SDK enumerations.
`carriedItems` counts valid copied carried identities; total polling also counts
the separately observed cursor, while enumeration does not. It is not a count
of ground loot. Unavailable-player attempts may report a poll without a scan.
Ground name, identity, rule resolution, visibility-service and existing minimap
render scopes are timed separately. `rulesAvailable` counts the size of each
rule table supplied to resolution, not rules evaluated before short-circuiting.
The minimap scope includes its own mutex/fence work, excludes the original
Present/VSync call, and is absent when the existing renderer hook is unavailable.

Prime one report, play for 30-60 seconds, then request another. Counts and total
times cover completed work since the previous report (first report: since load).
Average and total elapsed milliseconds, calls/second and elapsed ms/second are
reported per scope; `peakSinceLoad_ms` is the lifetime maximum. Timings include
callees and lock waits and overlap; do not sum nested scopes. Concurrent
completions near a report boundary can appear in neighboring intervals.
There are no per-item log messages, allocation or reporting locks on the
timing path. An empty timed scope measured about 0.05 us in an isolated fixture
on this host; actual game overhead and FPS improvement need live measurement.

Use a frame-time overlay alongside the log. Compare the same graphics settings,
frame cap, filter and plugin combination. Reproduce with only UnHoarder and
Charm Inventory before testing matching QOL. Test quiet and active map scenes,
with labels visible/hidden and with empty/full carried storage where practical.
With no filter or only text/name/background/hide rules, actual inventory scans
should remain zero. Sound, icon or sell-price features retain their existing
observation needs; the worker timer and filesystem watcher still wake normally.

## Validation

All 41 UnHoarder CTest cases and 51 Python source checks pass. The new
[performance fixtures](../tests/performance/CMakeLists.txt) compile the actual
production polling callbacks, cache invalidator and lookup/store code against
owned service copies and clocks. They prove no-filter/text-only inactivity;
all seven combinations of active features; cursor/custom-page coverage;
sound ID re-arming; queued reload and unavailable-player exits; one-slot
invalidation, collision safety, generation isolation and the existing TTL.

The no-filter and one-slot tests failed before the implementation change.
Matching QOL's actual-DLL admission test rejects each changed wrapper byte,
cleared original slot, and each changed trampoline/continuation byte after
warming identity admission. The game was not attached, changed or launched.
These are automated behavior/guard checks and isolated cost measurements;
the reported FPS regression, visible/audible acceptance and hosted behavior
remain unverified in game.
