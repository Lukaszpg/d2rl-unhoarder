# UnHoarder Tooltip Compatibility API v1

This is a deliberately small opt-in interface for plugin authors whose plugin
uses one of the same native tooltip/render functions as UnHoarder.

It is **not** a general-purpose hook-sharing framework. V1 covers only the two
shared render hooks that most directly affect UnHoarder's visible ground-item
styling on D2R build `93847`:

| Hook | RVA | Purpose |
| --- | ---: | --- |
| Shared label paint | `0x1FA8E0` | background color / visual suppression scope |
| Glyph renderer | `0x658510` | text color |

The public contract is:

`interop/unhoarder_tooltip_compat_v1.hpp`


## Standalone runtime

UnHoarder does not depend on Sanctuary of Exile or any SoE-specific observer,
style-transformer, render-scope, or DLL-export contract. The compatibility API
in this document is the opt-in mechanism for sharing the two covered native
render hooks with any cooperative D2RLoader plugin.

## The rule

Only one plugin physically hooks a supported native entry point.

The plugin that owns the physical hook publishes
`unhoarder-tooltip-compat` through D2RLoader's
`PluginCommunicationService`. Other compatible plugins register middleware
with that host instead of installing a second hook.

UnHoarder uses `DiagnosticsService` to identify a D2RLoader-tracked owner.
If the owner publishes this service, UnHoarder registers its own middleware
with it. If UnHoarder owns the hook, it publishes the same service so another
plugin can register with UnHoarder.

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
service from provider plugin ID `loot-filter`:

```cpp
D2RL::PluginCommunication::ServiceLease<
    UnHoarder::TooltipCompatV1::Service> lease;

D2RL::PluginCommunication::Acquire(
    context,
    communication,
    "loot-filter",
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
  service. A later plugin can diagnose `loot-filter` as owner and acquire it.
- If the other plugin loads first and owns the entry, it publishes the same
  service. When UnHoarder starts, Diagnostics identifies that plugin and
  UnHoarder registers with its host.

## Scope

V1 intentionally does not cover every native function used by UnHoarder.
Formatter/name-writer and hidden-hover row hooks retain their existing
fail-closed behavior. Additional hook points should be added only when a real
compatibility case requires them.
