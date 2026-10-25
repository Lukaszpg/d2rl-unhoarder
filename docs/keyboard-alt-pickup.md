# Keyboard ALT override for filter-hidden pickup

## Policy and independent operation

1.2.16 restores the formerly unregistered pickup guard for keyboard/mouse input.
A current Hide rule blocks the action even if the user clicks only the ground
gear icon. Either keyboard ALT key, held at dispatch time, overrides the block;
key press history, label toggle state, a controller button and timeouts do not.
No polling was added. Controller/unknown input passes through so QOL's existing
controller policy and settings retain their effect. No QOL service, DLL or load
order is required by this implementation.

The prior 1.2.7-1.2.15 builds intentionally left the dispatcher unhooked. This
change supersedes that policy; the earlier ability to click hidden ground items
was therefore expected. Label, background and hover rendering are unchanged.

## Build-specific native contract

The current game-image check verifies the x64 PE signature, machine, optional
header and image size. An earlier imported version also hard-required the
loader's PE timestamp `0x6AB3782C`. This was obsolete for D2RLoader builds
reporting a different timestamp (e.g. `0x6ACC8256`) while running the
same qualified 93847 game code. The timestamp gate was removed, but the
complete dispatcher-body fingerprint, item lookup and exact call-site
witnesses **remain mandatory**. Dispatcher
`0xFABE0` has the four-argument x64 ABI
`void(uint32 action, void* player, uint32 type, uint32 id)`. Entry admission now
requires the **complete original captured body** from QOL's already-reviewed
`loot_approach_profile.h`, copied into `native_pickup_profile.hpp` in this fork.
It no longer admits a current snapshot just because its first byte is a push.
The SDK owns hook restoration; there is no manual patch or generic hook chain.

Calls `0xFA115`, `0xFBF30` and `0x101ADF` witness the dispatcher;
`0xFACB9` witnesses item lookup `0x9A5D0` with prefix `4C 63 CA`.
Those qualify installation, not the identity of callers allowed to be blocked.
The game UI manager slot `0x3440170`, manager DWORD `+0xDC`, is reused from
QOL's reviewed native-input contract. Only a readable mode zero and stable
manager pointer qualify keyboard input. Mode one (controller), other values,
null or unreadable state pass through. These are game observations, not a
runtime dependency on QOL. No new game RVA was discovered.

At each action 22/type 4 request, validate active filtering, rules mode, original
code reader, actor type zero, no reentrancy, a current hidden-rule snapshot,
fresh lookup by requested ID, item type/ID and strictly on-ground mode 3.
Evaluate the normal ordered rules using fresh native properties; Unknown does
not authorize a block. Re-read the complete type/class/ID/mode header after
rule evaluation. Recheck ALT and keyboard input immediately before suppression.
Other actions and permitted requests forward once with unchanged arguments;
permitted pickup invalidates the existing vendor-price observation as before.
No item, save or native input state is written.

`LOOT_PICKUP_GUARD_READY` confirms installation, with
`override=keyboard-alt input=keyboard-only`. A refusal names the guard and leaves
native pickup available. `/unhoarder perf` reports `pickup-guard` and
`pickup-hidden-blocked`, alongside all previous metrics. This is not a claim
about automatic gold pickup or other native routes that bypass this dispatcher.

## Validation boundary

The test fixture compiles the actual production qualifier and dispatch callback.
It verifies gear-icon requests without label state, both ALT keys, release and
late press, controller/unknown input, failed reads, changing identity, visible
and Unknown rule decisions, exact forwarding and no extra non-pickup work.
Existing rule tests cover ordered matching and unknown properties. QOL's exact
new-artifact profile must pass live-code and forwarding mutation tests before
pair installation. These private-process tests do not prove mouse or controller
behavior in the running game; direct/approach pickup and FPS need live retesting.
