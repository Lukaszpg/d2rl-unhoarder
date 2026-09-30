"""Generic cooperative UnitStat reader interoperability contract."""
from pathlib import Path

r = Path(__file__).resolve().parents[1]
p = (r / "src/plugin.cpp").read_text()
h = (r / "interop/unit_stat_read_compat_v1.hpp").read_text()

assert '"unit-stat-read-compat"' in h
assert "SupportedEntryRva = 0x002F5020ULL" in h
assert "std::uintptr_t ownerTarget;" in h
assert "ReadStatFn readStat;" in h
assert "sizeof(Service) == 32" in h
assert "soe" not in h.lower()

q = p[p.index("constexpr std::int32_t GroundQuantityStatId"):
      p.index("bool GroundCandidatePageReadable")]
assert "D2RLoaderGetPluginInfo" in q
assert "D2RL::GetPluginInfoFn" in q
assert "PluginCommunication::Acquire" in q
assert "UnitStatCompat::ServiceName" in q
assert "service->ownerTarget!=target" in q
assert "readerOwner!=owner" in q
assert "GroundQuantityCompatLease=std::move(lease)" in q
assert "LOOT_QUANTITY_COMPAT_READY" in q
assert "reader-owner-no-compatible-service" in q
assert '"soe"' not in q.lower()

# Direct D2RCore remains the preferred standalone route. The compatibility
# service is consulted only for a foreign owner of the already-qualified slot.
assert 'const auto d2rCore=GetModuleHandleW(L"D2RCore.dll")' in q
assert "if (owner!=d2rCore)" in q
assert "TryAcquireCooperativeGroundQuantityReader(owner,target)" in q

# A held service lease is dropped before the plugin context is invalidated.
unload = p[p.index("D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin"):]
assert unload.index("GroundQuantityReader.store(nullptr") < unload.index("GroundQuantityCompatLease.Reset()")
assert unload.index("GroundQuantityCompatLease.Reset()") < unload.index("Context = nullptr")

print("PASS generic UnitStat reader interoperability; standalone D2RCore route retained")
