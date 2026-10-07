from pathlib import Path

root = Path(__file__).resolve().parents[1]
src = root.joinpath("src/plugin.cpp").read_text()
rules = root.joinpath("src/filter_rule_engine.hpp").read_text()

# Public schema / rule-engine surface.
assert "bool sellPriceKnown{};" in rules
assert "std::uint32_t sellPrice{};" in rules
assert "NumberTest sellPrice;" in rules
assert "NextProperty::SellPrice" in rules
assert 'key=="sellPrice"' in src
assert "rule.conditions.sellPrice.enabled" in src
assert "fresh->usesSellPrice=true" in src
assert "propertySellPrice=%u" in src

# Build-93847 backend is the route verified against the game's own SELL caller
# and against Malah's actual offered gold.
for needle in (
    "VendorSellPriceMalahNpcId=513",
    "VendorSellPriceTransaction=1",
    "VendorSellPriceTransactionThunkRva=0x36F0B0",
    "VendorSellPriceTransactionImplementationRva=0x36F0C0",
    "VendorSellPriceDifficultyGetterRva=0x8AF40",
    "VendorSellPriceQuestFlagsSlotRva=0x2A48778",
    "VendorSellPriceWitnessQuestLoadRva=0x10D4F0",
    "VendorSellPriceWitnessDifficultyCallRva=0x10D4F7",
    "VendorSellPriceWitnessDataContextCallRva=0x10D4FF",
    "VendorSellPriceWitnessLocalPlayerCallRva=0x10D506",
    "VendorSellPriceWitnessTransactionArgRva=0x10D50E",
    "VendorSellPriceWitnessVendorArgRva=0x10D519",
    "VendorSellPriceWitnessTransactionCallRva=0x10D523",
):
    assert needle in src

# Exact witnesses still fail closed before native pricing is enabled.
assert "VendorSellPriceTransactionThunkExpected" in src
assert "VendorSellPriceTransactionImplementationExpected" in src
assert "VendorSellPriceWitnessTransactionArgExpected" in src
assert "VendorSellPriceWitnessVendorArgExpected" in src
assert "questSlotAddress!=Base+VendorSellPriceQuestFlagsSlotRva" in src
assert "difficultyAddress!=Base+VendorSellPriceDifficultyGetterRva" in src
assert "transactionThunkAddress!=Base+VendorSellPriceTransactionThunkRva" in src
assert "LOOT_SELL_PRICE_REFUSED reason=93847-sell-route-fingerprint" in src
assert "LOOT_SELL_PRICE_REFUSED reason=93847-sell-route-target-mismatch" in src

# Price is a lazy staged property, not a formatter-side diagnostic.
ground = src[src.index("RuleEngine::Item GroundRuleItem"):
             src.index("RuleEngine::Item CachedGroundRuleItem")]
assert "NextProperty::SellPrice" in ground
assert "ReadNativeVendorSellPrice" in ground
assert "item.sellPriceKnown=true" in ground
assert "item.sellPrice=sellPrice" in ground
assert "ObserveVendorPriceCandidate" not in src
assert "LOOT_VENDOR_PRICE_PROBE_ITEM" not in src
assert "diagnosticOnly=1" not in src

# Unidentified magic/rare are deliberately priceable: the native transaction
# calculator suppresses affix/bonus-stat price contributions until identified,
# so this exposes only the visible/base-state lower bound.
safe = src[src.index("bool VendorSellPriceSafeQuality"):
           src.index("bool VendorSellPriceContext")]
for quality in ("properties.quality==1U", "properties.quality==2U",
                "properties.quality==3U", "properties.quality==4U",
                "properties.quality==6U"):
    assert quality in safe
assert "properties.identifiedKnown && properties.identified" not in safe
assert "unidentifiedMagicRare=base-only" in src
assert "unidentifiedMagicRare=unknown" not in src
assert "excludes affix/bonus-stat contributions until the" in src

# Cache is short-lived and additionally forgotten on pickup/inventory/cursor
# observation so quest-price changes and modified re-drops cannot stay stale.
assert "ResetVendorSellPriceSession();" in src
assert "ForgetVendorSellPriceItem(id);" in src
assert "ForgetVendorSellPriceItem(item->runtimeId);" in src
assert "ForgetVendorSellPriceItem(info.runtimeId);" in src
assert "TryGetCachedVendorSellPrice" in src
assert "StoreVendorSellPrice" in src
assert "VendorSellPriceCacheTtlMs=1000" in src
assert "now-entry.observedMs<=VendorSellPriceCacheTtlMs" in src
assert "entry.nativeUnit==reinterpret_cast<std::uintptr_t>(nativeUnit)" in src
assert "scalars.sellPriceKnown,scalars.sellPrice" in src
assert "scalarOut->sellPriceKnown=candidate.sellPriceKnown" in src
assert "NativeRowLiveLatestLabel.sellPriceKnown=item.sellPriceKnown" in src
assert "candidate.sellPriceKnown=label.sellPriceKnown" in src
assert "rowItem.sellPriceKnown=append.sellPriceKnown" in src

# Native failures make the property unknown/fail-open.
assert "VendorSellPriceQualified.store(false" in src
assert "LOOT_SELL_PRICE_DISABLED reason=native-call-fault failClosed=1 retry=next-game" in src
assert "VendorSellPriceRequalifyNextGame.store(true" in src
assert "VendorSellPriceRequalifyNextGame.exchange(" in src
joined = src[src.index("void __cdecl OnInWorldGameJoined"):
             src.index("void RegisterInWorldLifecycle")]
assert "RefreshVendorSellPriceForGame();" in joined
assert "VendorSellPriceRequalifyNextGame" not in joined
assert "void RefreshVendorSellPriceForGame() noexcept {" in src
qualify = src[src.index("void QualifyVendorSellPrice() noexcept {"):
              src.index("// Qualified native hover-row renderer")]
assert "if(!VendorSellPriceTransactionFnPtr)" in qualify
assert "VendorSellPriceTransactionFnPtr=nullptr" not in qualify
unload = src[src.index("D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin"):]
assert "VendorSellPriceTransactionFnPtr=nullptr" not in unload
assert "VendorSellPriceGetDifficulty=nullptr" not in unload
assert "LOOT_SELL_PRICE_READY" in src

# Old qualification strategy and exploratory naming are gone.
for stale in (
    "VendorPriceProbe",
    "VendorPriceGetQuestInfo",
    "VendorPriceGetGame",
    "VendorPriceQuestInfoCallPattern",
    "VendorPriceGameCallPattern",
    "LOOT_VENDOR_PRICE_PROBE",
):
    assert stale not in src

print("production vendor sell-price contract: ok")
