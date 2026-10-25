# Shared-label preflight: UnHoarder 1.2.12 / QOL rev.85

The user isolated a later regression to **both** plugins enabled: 15 town gems
cost about 35 FPS when labels were shown, while either plugin alone did not.
This is a stronger combined-path reproducer than the earlier inventory-poll
hypothesis. The old live reports show costly nested QOL label checks, but do
not independently establish how much of the total FPS loss they explain.

The shared path is `ObserveCompletedName -> QolClientLoot::Track ->
HiddenForPickup -> Validate/ReadPickupCode/HiddenCandidate ->
unhoarder-visibility.query -> GroundRuleItem`. It runs even in keyboard/mouse
mode. `Track` must maintain the existing same-session, 250 ms Hidden proof:
`HookPickup` uses it to prevent filtered items being taken with controller A.
Deleting that refresh or caching successful native admission is not safe.

Rev.85 passes the already-borrowed label unit to a separate leased service,
`unhoarder-label-visibility` v1, owned by provider `unhoarder`. Its standard-layout
16-byte table contains uint32 size/version at offsets 0/4 and one cdecl noexcept
callback at offset 8: `Decision(const void* unit, uint32_t guid)`. The uint32
enum is `RequireQuery=0`, `CannotHide=1`. The callback and unit are borrowed only
for the synchronous call; no table or native pointer is retained. Acquire and
release use the existing SDK broker lifetime. Publication/unload follows the
existing provider service lifecycle. Either load order and an older provider
work: the service is acquired afresh and absence falls back to the full query.

The new service can only prove **no Hide rule is possible**. It cannot create a
Hidden proof or authorize an action. UnHoarder re-reads the 16-byte native header
(type/class/id/mode), requires item type 4, supplied id and ground mode 3, reads
the current canonical native code, and evaluates all Hide rules with other
properties unknown. Type/base selectors are already expanded to code sets by
the existing parser. Any Match or Unknown keeps the full query required;
rule order and Continue never justify an early skip. A positive proof rechecks
code, complete header, rule snapshot and presentation arming. The original
inactive-Hide/geometry-off Visible outcome also clears the proof. No mutable
property, successful admission, page information or rule result is cached.

`CannotHide` only erases an old QOL Hidden proof and updates the same label
observation timestamp. Every other result (including unknown enum, missing or
invalid service and missing unit) runs the original `HiddenForPickup` unchanged.
Native A-button enforcement, full service v1, Submit, session/age/epoch fences,
local-actor and one-use permits, gold exemptions and all action guards remain.
Rendering, rule output, sounds, polling cadence and reticle behavior are unchanged.
The user has now confirmed the reticle works; the earlier gate-2 diagnostic was
captured in keyboard/mouse mode and is no longer an unresolved reticle defect.

This reduces work for gems and other codes excluded by all active Hide rules.
Potentially hidden equipment and generic property-only Hide rules still take
the full path (plus a small preflight); no universal FPS improvement is claimed.
The original full validator remains mandatory whenever it is needed.

### Exact paired artifact

UnHoarder **1.2.12**, SHA256
`2c8aa235f55990250fcf9da03e611591fad1485286494a6a2e90683a4c9f6f29`;
image size `0xE8000`, forwarding wrapper RVA `0x1F4E0`, original slot `0xB5938`.
Release link-map symbols and DLL bytes establish the same one-argument,
unchanged-return forwarding contract:

```text
48 83 EC 28 48 8B 05 4D 64 09 00 FF D0 90 48 83 C4 28 C3
```

QOL's additional exact validator retains
the full game `0x36EF50` entry/tail, relay, wrapper, original slot, trampoline,
continuation and page/ownership checks. Only reviewed coordinates/bytes differ;
all ten older exact artifact profiles remain. There are no new game RVAs or hooks.

### Evidence and monitoring

The extracted production Track regression failed before the optimization:
1,800 fallback queries for 15 labels over 120 iterations. The green test skips
all 1,800, exercises leased-provider refusal and keeps proof/session behavior.
The independent provider test covers 7,680 completed-rule parity cases and
changed identity, code, snapshot and arming. An owned combined fixture compiles
both actual callbacks through the SDK lease helper: 1,800 legacy queries versus
zero for no-Hide gems, with full-query fallbacks on reload, identity mutation,
provider loss and potentially hidden equipment. Native full-query execution is
a spy in that fixture; these counts are not measured game frames or FPS.

`qol perf` adds `label-visibility-preflight` and `label-no-hide-query-skipped`;
`unhoarder perf` adds `label-visibility-preflight` and `label-no-hide-proven`.
The former timings include nested work. Skipped/proven are event counts with
zero-duration records. Only active Rules proofs increment UnHoarder's proven
counter; QOL's skip count also includes inactive filtering. Existing counters
and command interval semantics are unchanged. Prime both reports, show the
same 15 gems for 30 seconds, report both again and record actual FPS separately.
The no-Hide skip count should track gem-label observations, while full
`visibility-service` and compatibility counts should fall sharply when idle.

Build, artifact and automated checks are recorded with the local candidate.
Live combined FPS recovery, controller hidden pickup, visible styling and
filter/session transitions still require acceptance with this exact pair.
