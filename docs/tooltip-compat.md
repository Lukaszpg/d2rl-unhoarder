# UnHoarder Plugin Interoperability

This document describes the opt-in contracts available to D2RLoader plugin
authors whose plugin owns or shares a native boundary also used by UnHoarder.

The contracts are deliberately narrow. They are not a general-purpose
hook-sharing framework, and every cooperative route remains tied to a
qualified build-`93847` native boundary and its diagnosed D2RLoader owner.

## Tooltip render middleware

The **UnHoarder Tooltip Compatibility API v1** covers the two shared render
hooks that most directly affect visible ground-item styling:

| Hook | RVA | Purpose |
| --- | ---: | --- |
| Shared label paint | `0x1FA8E0` | background color / visual suppression scope |
| Glyph renderer | `0x658510` | text color |

The public contract is:

`interop/unhoarder_tooltip_compat_v1.hpp`


## Common trust model

UnHoarder has no mod-specific DLL/export dependency for these routes. The
common rule is **owner first, service second**:

1. qualify the native boundary for the supported game build;
2. use D2RLoader `DiagnosticsService` to identify the tracked owner;
3. acquire the versioned service from that exact provider through
   `PluginCommunicationService`;
4. validate the service-specific ownership fields and keep its lease while any
   callback or function pointer can be used.

A DLL name, plugin load order, matching product name, or executable-looking
foreign pointer is not sufficient evidence. If the owner cannot be proved, the
service is absent, or the service does not match the qualified boundary,
UnHoarder fails that feature closed and leaves unrelated features active.

## The rule

Only one plugin physically hooks a supported native entry point.

The plugin that owns the physical hook publishes
`unhoarder-tooltip-compat` through D2RLoader's
`PluginCommunicationService`. Other compatible plugins register middleware
with that host instead of installing a second hook.

UnHoarder uses `DiagnosticsService` to identify a D2RLoader-tracked owner.
It then enumerates the exact modification ranges and accepts a foreign host only
when the entry itself is a loader-managed `InlineHook` from that same single
owner and Diagnostics reports `callThrough == Yes`. If the owner publishes
this service, UnHoarder registers its own middleware with it. If UnHoarder owns
the hook, it publishes the same service so another plugin can register with
UnHoarder.

If the modified entry is untracked, has multiple owners, or the owner does not
publish this contract, UnHoarder does not guess or decode a foreign detour. The
affected feature fails closed while unrelated UnHoarder features remain active.

## Requirement for a host plugin

Install the conflicting native hook through D2RLoader
(`PluginContext::InstallInlineHook` or the loader mutation service). Do not use
a private MinHook detour for a hook you expect UnHoarder to discover: an
untracked modification intentionally has no trusted owner identity.

Publish a `UnHoarder::TooltipCompatV1::Service` under your own plugin ID using
`PluginCommunicationService::publishService` and the exact service name
`unhoarder-tooltip-compat`.

A middleware callback receives the native caller return address, the native
arguments, and a `next` continuation. It may modify arguments before calling
`next`. It may call `next` zero or one time. Calling it zero times suppresses
the rest of that render invocation. A host must guard against a callback
calling `next` more than once.

The callback and all pointers passed to it are synchronous borrowed values.
Never retain them.

A successful `unregisterSharedLabelPaint` or `unregisterGlyphRenderer` is a
**quiescence barrier**. Once it returns, that registration must not be executing
and no old dispatch snapshot may begin another call into it. This guarantee is
required so a consumer can safely release its service lease and unload its DLL.

Do not unregister a registration from inside that registration's own active
callback. UnHoarder returns `Result::Unsupported` for that case to avoid a
self-deadlock. Unregister after the callback returns.

## Minimal host shape

A plugin that already owns `0x1FA8E0` can keep one or more registered
`SharedLabelPaintMiddlewareFn` callbacks. Its native hook should dispatch the
middleware chain and make the original/trampoline call the terminal
continuation.

Conceptually:

```cpp
void __fastcall MySharedLabelPaintHook(
    void* rect, void* textArg, void* colorArg) noexcept
{
    const auto caller =
        reinterpret_cast<std::uintptr_t>(_ReturnAddress());

    // Dispatch registered middleware. The final next() calls
    // MyOriginalSharedLabelPaint(rect, textArg, colorArg).
    DispatchCompatPaintChain(caller, rect, textArg, colorArg);
}
```

The glyph hook follows the same pattern and returns the value returned by the
terminal continuation.

## Consumer shape

When another plugin sees that UnHoarder owns a supported entry, acquire the
service from provider plugin ID `unhoarder`:

```cpp
D2RL::PluginCommunication::ServiceLease<
    UnHoarder::TooltipCompatV1::Service> lease;

D2RL::PluginCommunication::Acquire(
    context,
    communication,
    "unhoarder",
    UnHoarder::TooltipCompatV1::ServiceName,
    UnHoarder::TooltipCompatV1::ServiceVersion,
    UnHoarder::TooltipCompatV1::ServiceRequiredSize,
    &lease);
```

Then register only the middleware you need. Keep the service lease while the
registration is live. During your plugin unload, unregister the callback before
releasing the lease.

There is no hard DLL import and no `GetProcAddress` dependency.

## Load order

No fixed load order is required for the normal collision case:

- If UnHoarder loads first, it owns the vanilla entry and publishes the host
  service. A later plugin can diagnose `unhoarder` as owner and acquire it.
- If the other plugin loads first and owns the entry, it publishes the same
  service. When UnHoarder starts, Diagnostics identifies that plugin and
  UnHoarder registers with its host.

## Cooperative UnitStat reader

Header:

`interop/unit_stat_read_compat_v1.hpp`

Service:

`unit-stat-read-compat` ABI v1

Native boundary:

`D2R+0x2F5020`

This contract is for a plugin that legitimately owns/chains the qualified
UnitStat reader bridge and wants consumers to read through that owner rather
than attempting a second hook or calling an unknown target.

A provider publishes `D2RLInterop::UnitStatReadCompatV1::Service` under its
own D2RLoader plugin ID. The service identifies the supported entry RVA, the
provider's exact live owner target, and a synchronous `readStat` callback.

UnHoarder first qualifies the loader bridge independently. Its normal
standalone path still requires the direct target to be `D2RCore.dll`. If the
live target belongs to another plugin, UnHoarder resolves that module's
D2RLoader plugin ID, acquires `unit-stat-read-compat` from that provider, and
accepts it only when:

- the service ABI/size and entry RVA match;
- `ownerTarget` is exactly the bridge target UnHoarder observed;
- the `readStat` callback belongs to the same provider module;
- the service lease remains held while the callback can be invoked.

Consumers should reacquire per game when their native qualification is
per-game. UnHoarder does this on every `GameJoined`.

## Cooperative in-world item-label host

Header:

`interop/in_world_item_label_compat_v1.hpp`

Service:

`in-world-item-label-compat` ABI v1

Native boundary:

`D2R+0xC0420`

This contract covers the separate in-world item-label path used for item hover
when normal/bulk ground labels are hidden. A plugin that already owns the
qualified `0xC0420` path can publish a service so consumers do not install a
competing detour.

The service exposes three synchronous capabilities:

- **observer** — receives the borrowed live item identity and source label;
- **transformer** — may return replacement label bytes for that same event;
- **active-item scope** — reports the currently executing same-thread item so a
  consumer can correlate downstream native rendering safely.

A host must treat observer/transformer registration and unregistration as
lifetime-sensitive. Successful unregister must be a quiescence barrier: after
it returns, no callback from that registration may still be executing or start
from an old dispatch snapshot. Borrowed event, unit, source, and active-item
pointers must never be retained after the synchronous call.

UnHoarder discovers the actual tracked owner of `0xC0420`, acquires this
service from that exact plugin ID, and uses it only as identity/scope evidence.
Its hidden-hover background/text/Show:false behavior still independently
qualifies the downstream native row append and renderer chain. Publishing this
service therefore does **not** authorize arbitrary render hooks or bypass
UnHoarder's native fingerprints.

## Load order for provider-local services

No product-specific load order is required. The plugin that owns the native
boundary publishes the corresponding provider-local service. A consumer
discovers that owner and acquires from it.

If the service is not ready at the moment a consumer checks, the consumer
should fail closed and retry at an appropriate lifecycle boundary rather than
guessing the provider. UnHoarder retries its per-game cooperative readers on
`GameJoined`.

## Scope

Interoperability is intentionally added one proven collision at a time.
UnHoarder's current public contracts cover the two tooltip render hooks, the
qualified UnitStat bridge, and the in-world hidden-hover item-label host.
Uncovered native collisions retain fail-closed behavior.
