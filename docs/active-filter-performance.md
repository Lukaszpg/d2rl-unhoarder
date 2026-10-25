# Active-filter cost and 1.2.11

The next candidate is documented in [shared-label performance](shared-label-performance.md).

The 1.2.10 no-filter polling fix is confirmed by live logs. A later active
13-rule filter and approximately 115-second capture still reported about
40 FPS, recovering when the player left the loot area and dropping on return.
The user also suspects colored labels. This is a reproducible scene-dependent
symptom; it does not establish a single renderer or property as its cause.

## Behavior-preserving changes

`GroundRuleItem` has a geometry-only demand flag used solely by the qualified
inner name writer. Before each staged native property group, the portable
`GroundActionDemand::GeometryPossible` checks all name/icon actions using the
existing three-state condition evaluator. Unknown facts remain possible. Only
if every such action is proven unable to match does that path stop collecting
unrelated fields. Its eventual name/icon output is then necessarily empty;
the existing resolver, quantity prefix and marker-clear behavior still run.
Continue, later Hide blocks, partial observations and unavailable properties
remain conservative. No native value or successful guard is cached.

Identity capture, visibility queries, painting, drop sounds, pricing and pickup
retain their original property requirements and collection paths. Every property
that is actually read retains its native identity, mode, page, range, repeated
snapshot and fail-open predicates. The socket capability still reads stat 194
twice, checks the identity around each observation, retains the installed game
bridge and Maps hook and validates the complete reviewed graphs before and
after **each** native read. All four guard invocations remain.

The Maps image reader now copies behind an SEH fault boundary after its
unchanged per-read page/kind/ownership/section loop. Raw PE/bridge/slot copies
outside that qualified loop retain `ReadProcessMemory`; no raw borrowed item
read was replaced. PE metadata and every reachable graph witness are re-read
on every guard. A broad replacement that added new page queries was measured
and rejected because it slowed socket guards. No changes to rendering colors,
hook sites, native call counts, scalar caching or pickup policy are included.

## Measurements and validation

The owned exact Maps/Core fixture measured four socket guards at 118.725 us
before and 99.650 us after (about 16% less), 32 rather than 68 cross-process API
reads and the same 36 live page queries. Native stat calls are excluded; these
measurements do not predict FPS. The production geometry fixture compares
3,456 full/pruned cases across quality, sockets, ethereal, unavailable fields,
Continue and name/icon composition. It proves ordinary no-action items skip
unneeded socket observations while full visibility/identity collection remains,
and refuses changed identity/quality or unreadable data after warmup.

All 43 CTest cases and 51 Python source checks pass. There is no new live-game
FPS, visible styling, alert or pickup acceptance for 1.2.11 yet. Its exact
matching QOL pair is rev.84, which also retains the unreleased reload changes.

The four extra `unhoarder perf` metrics are nested elapsed scopes:

- `native-properties`: the complete staged property collection, including rules.
- `socket-observation`: both native stat reads and their complete guard work.
- `ground-style`: UnHoarder's own qualified style selection, excluding forwarded
  native drawing and glyph rendering.
- `glyph-forward`: glyph middleware including downstream/native forwarding;
  it is not a pure measurement of UnHoarder overhead.

Do not sum nested times or infer FPS/GPU cost from callback timing. Existing
report names, interval boundaries, peaks and read-only semantics are retained.

## Exact candidate identity

UnHoarder 1.2.11 SHA256:
`bc98e70a31c7765b9e25219857aed623eca507a27da3a95cbafb0241a9ce723c`.
Image size `0xE7000`; `HookGetItemCode` RVA `0x1EC30`, original pointer slot
`0xB48B8`. The Release link map and DLL give the full wrapper:

```text
48 83 EC 28 48 8B 05 7D 5C 09 00 FF D0 90 48 83 C4 28 C3
```

It forwards the same one argument once and returns the original result. QOL
retains the complete original game entry/tail, relay, module/page ownership,
wrapper, slot, trampoline and native continuation proof. A rebuild with a
different hash requires independent review. No new game function or ABI was introduced.
