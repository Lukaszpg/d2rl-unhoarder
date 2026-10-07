from pathlib import Path

src = Path(__file__).resolve().parents[1].joinpath("src/plugin.cpp").read_text()

# Startup may begin with no valid rules. A corrected file must be allowed to
# become the first active filter without a restart.
assert "requires-previously-active-filter" not in src
assert "action=restart-with-valid-json" not in src
assert "QueueFirstFilterActivation" in src
assert "ActivateFirstFilterOnGameThread" in src
assert "FirstFilterActivationPending" in src

# Parsing remains on the worker, but first native hook activation is dispatched
# through D2RLoader's game-thread service.
queue = src[src.index("bool QueueFirstFilterActivation"):
            src.index("// The worker owns filesystem observation")]
assert "ReloadFilterRules()" in queue
assert "runOnGameThread" in queue
assert "ActivateConfiguredFilter(false)" not in queue

callback = src[src.index("void __cdecl ActivateFirstFilterOnGameThread"):
               src.index("bool QueueFirstFilterActivation")]
assert "ActivateConfiguredFilter(false)" in callback
assert "GroundIdentities.fill({})" in callback
assert "PublishedFilterRules.store(request->previous" in callback
assert "FilterLiveReloadAvailable.store(false" in callback
assert "first-activation-unavailable" in callback

# Once a filter is active, the existing in-place reload path remains intact
# and preserves the last valid snapshot on failure.
reload = src[src.index("bool TryLiveFilterReload"):
             src.index("// Native IDs can be recycled")]
assert "FilterLiveReloadAvailable.load" in reload
assert "return QueueFirstFilterActivation(trigger,previous)" in reload
assert "PublishedFilterRules.store(previous" in reload
assert "reactivation-unavailable" in reload
assert "LogReloadSuccess(trigger,previous,current,false)" in reload

# The first-activation request owns the exact parsed immutable candidate and
# refuses stale/superseded publication before arming hooks.
assert "current!=request->candidate" in callback

print("first valid filter live activation contract: ok")
