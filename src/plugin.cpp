#include <D2RLPlugin/api.h>
#include <D2RLPlugin/item.h>
#include <D2RLPlugin/inventory.h>
#include <D2RLPlugin/lifecycle_events.h>
#include "ground_quantity_label.hpp"
#include "ground_visibility_policy.hpp"
#include "native_pickup_guard_policy.hpp"
#include "filter_rule_engine.hpp"
#include "base_name_table.hpp"
#include "item_type_table.hpp"
#include "ground_property_reader.hpp"
#include "minimap_world_position.hpp"
#include "automap_projection.hpp"
#include "minimap_overlay_renderer.hpp"
#include "minimap_icon_policy.hpp"
#include "ground_property_live_policy.hpp"
#include "ground_ethereal_policy.hpp"
#include "ground_identified_policy.hpp"
#include "filter_live_reload.hpp"
#include "ground_sound_registry.hpp"
#include "named_sound_loader_identity.hpp"
#include "../interop/unhoarder_tooltip_compat_v1.hpp"
// D2RLoader PluginSDK ThreadService is queried through its typed service contract.
#include <D2RLPlugin/threads.h>
#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <initializer_list>
#include <system_error>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <thread>
#include <stop_token>
#include <limits>

namespace SoE::LootFilter {
namespace {

// Qualifies against the 93847 process, not a declaration that this is a label function.
// D2 item codes occupy four native bytes, but many Excel codes have only
// three printable characters (e.g. "exo"). Keep the visible code unchanged
// and canonicalize only trailing NUL/space padding for JSON comparisons.
// Do not write the canonicalized value into a native item or unit.
constexpr std::uint32_t CanonicalItemCode(std::uint32_t raw) noexcept {
    for (int index = 3; index >= 0; --index) {
        const unsigned shift = static_cast<unsigned>(index) * 8U;
        const auto byte = static_cast<unsigned char>(raw >> shift);
        if (byte != 0U && byte != static_cast<unsigned char>(' ')) break;
        raw &= ~(0xFFU << shift);
    }
    return raw;
}

constexpr std::uintptr_t GetItemCodeRva = 0x36EF50;
constexpr std::array<std::uint8_t, 32> ExpectedGetItemCode{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48, 0x85, 0xC9,
    0x75, 0x13, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D,
    0x4C, 0x24, 0x30, 0xE8, 0x80, 0x83, 0xFF, 0xFF,
};
constexpr std::size_t CandidateTextMaximum = 96;
constexpr std::uintptr_t LabelFormatterRva = 0x1FA9F0;
// _ReturnAddress() points AFTER the 5-byte CALL, not at the CALL opcode.
// Two verified call sites enter 0x1FA9F0 in the ground-label producer body.
constexpr std::uintptr_t LabelFormatterCallRva = 0x15171E5;
constexpr std::uintptr_t LabelFormatterSecondCallRva = 0x1517783;
constexpr std::size_t DirectCallBytes = 5;
constexpr std::array<std::uint8_t, 16> ExpectedLabelFormatter{
    0x40,0x55,0x53,0x56,0x41,0x54,0x41,0x57,
    0x48,0x8B,0xEC,0x48,0x83,0xEC,0x50,0x45
};
constexpr std::array<std::uint8_t, 5> ExpectedLabelFormatterCaller{
    0xE8,0x06,0x38,0xCE,0xFE
};
constexpr std::array<std::uint8_t, 5> ExpectedLabelFormatterSecondCaller{
    0xE8,0x68,0x32,0xCE,0xFE
};
// The formatter calls 0xCBEB0 at 0x1FAA18, then performs its native
// text measurement in 0x909560/0x902990 BEFORE returning to our outer hook.
// Intercepting this one call lets D2R measure a replacement of any length;
// changing record+0x28 in the outer post-hook cannot do that.
constexpr std::uintptr_t InnerNameWriterRva = 0xCBEB0;
constexpr std::uintptr_t InnerNameWriterCallerRva = 0x1FAA18;
constexpr std::array<std::uint8_t,19> ExpectedInnerNameWriter{
    0x40,0x53,0x55,0x56,0x57,0x48,0x81,0xEC,0x68,0x04,
    0x00,0x00,0x48,0x8B,0x05,0x05,0xF4,0x8F,0x02
};
constexpr std::array<std::uint8_t,5> ExpectedInnerNameCall{
    0xE8,0x93,0x14,0xED,0xFF
};
constexpr std::size_t InnerNameBufferBytes = 0x80; // observed r8d=0x80
constexpr std::size_t InnerNamePrefixBytes = 4; // text at record+0x28
enum class GeometryMode : std::uint32_t { Off, Rules };

using GetItemCodeFn = std::uint32_t(__fastcall*)(void*) noexcept;
using LabelFormatterFn = std::uint8_t(__fastcall*)(
    void*, void*, void*, std::uint32_t, std::uint64_t, std::uint64_t) noexcept;
LabelFormatterFn OriginalLabelFormatter{};
std::atomic_bool FormatterHookInstalled{};
std::atomic<std::uint64_t> FormatterCalls{};
const D2RL::PluginContext* Context{};
std::uintptr_t Base{};
std::uint32_t ImageSize{};
GetItemCodeFn OriginalGetItemCode{};
std::atomic_bool HookInstalled{};
using InnerNameWriterFn = std::uint64_t(__fastcall*)(
    void*, void*, std::uint32_t, void*) noexcept;
InnerNameWriterFn OriginalInnerNameWriter{};
std::atomic_bool InnerNameHookInstalled{};
std::atomic<GeometryMode> ActiveGeometryMode{GeometryMode::Off};
std::atomic<std::uint64_t> GeometryTotalCalls{};
std::atomic<std::uint64_t> GeometrySourceCalls{};
std::atomic<std::uint64_t> GeometryValidPairs{};
std::atomic<std::uint64_t> GeometryIdMismatches{};
std::atomic<std::uint64_t> GeometryLookups{};
std::atomic<std::uint64_t> GeometryNoRule{};
std::atomic<std::uint64_t> GeometryReadFailures{};
std::atomic<std::uint64_t> GeometryNoTerminator{};
std::atomic<std::uint64_t> GeometryWrites{};
std::atomic<std::uint64_t> GeometryObserved{};
// An immutable, atomically published code-to-name snapshot. The hook may
// inspect it without file I/O, parsing, locks held across original calls,
// or consulting a mutable vector during reload.
struct FilterNameRule {
    bool schema2{};
    RuleEngine::Conditions conditions{};
    std::uint32_t code{};
    std::array<char, CandidateTextMaximum> name{};
    std::size_t bytes{}; // ASCII byte count INCLUDING NUL when hasName
    bool hasName{};
    bool show{true}; // block disposition: Show=true, Hide=false
    bool continueEvaluation{}; // PoE-style Continue: apply this block, then keep evaluating
    bool hasBackground{};
    std::array<float,4> background{}; // parsed RGBA, valid for table lifetime
    bool hasTextColor{};
    std::array<float,4> textColor{}; // forwarded to glyph B, no record writes
    bool hasDropSound{};
    std::array<char,64> dropSound{}; // sounds.txt row name (NOT numeric index)
    bool hasMinimapIcon{};
    MinimapOverlayRenderer::Shape minimapShape{MinimapOverlayRenderer::Shape::Diamond};
    std::array<float,4> minimapBorderColor{};
    std::array<float,4> minimapFillColor{};
    float minimapSizePx{MinimapIconPolicy::DefaultSizePx};
};
struct FilterRuleTable {
    std::vector<FilterNameRule> rules;
    std::uint32_t schema{1};
    std::filesystem::path baseNamesExcelPath{}; // reload-time source for worker watching only
    std::filesystem::path itemTypesExcelPath{}; // same source, only when itemType used
    bool usesQuantity{};
    bool usesQuality{};
    bool usesItemLevel{};
    bool usesSockets{};
    bool usesEthereal{};
    bool usesIdentified{};
    bool usesItemType{};
    std::uint64_t generation{};
    std::size_t backgroundRules{};
    std::size_t textColorRules{};
    std::size_t soundRules{};
    std::size_t minimapIconRules{};
    std::size_t hiddenRules{};
};
// Effective action state after evaluating an ordered Show/Hide chain.
// Continued blocks compose actions; later matching blocks override only the
// fields they explicitly provide. No Conditions vectors are copied here.
struct GroundRuleDecision {
    bool show{true};
    bool hasName{};
    std::array<char, CandidateTextMaximum> name{};
    std::size_t bytes{};
    bool hasBackground{};
    std::array<float,4> background{};
    bool hasTextColor{};
    std::array<float,4> textColor{};
    bool hasDropSound{};
    std::array<char,64> dropSound{};
    bool hasMinimapIcon{};
    MinimapOverlayRenderer::Shape minimapShape{MinimapOverlayRenderer::Shape::Diamond};
    std::array<float,4> minimapBorderColor{};
    std::array<float,4> minimapFillColor{};
    float minimapSizePx{MinimapIconPolicy::DefaultSizePx};
};
std::shared_ptr<const FilterRuleTable> PublishedFilterRules{};
std::filesystem::path FilterConfigPath;
std::atomic<std::uint64_t> FilterGeneration{};
std::atomic<std::uint64_t> FilterLookups{};
std::atomic<std::uint64_t> FilterMatches{};
std::atomic<std::uint64_t> FilterNoRule{};
std::atomic<std::uint64_t> FilterWrites{};
std::atomic<std::uint64_t> FilterGuardFailures{};
std::atomic<std::uint64_t> FilterReloadSucceeded{};
std::atomic<std::uint64_t> FilterReloadRefused{};
std::atomic_bool FilterLiveReloadAvailable{};
constexpr std::size_t MaximumFilterFileBytes = 4 * 1024 * 1024;
constexpr std::size_t MaximumFilterRules = 4096;
constexpr std::size_t MaximumFilterNameBytes = 79; // measured in BYTES (no NUL)
// Build-93847 ground-label background contract: record+0x14 is the native
// float[4] RGBA passed by the qualified ground-label route into D2R+0x1FA8E0.
// UnHoarder forwards a stack-local replacement pointer for the synchronous
// draw only; it never persists color data into the native label record.
constexpr std::uintptr_t GroundColorOffset = 0x14;
constexpr std::array<float,4> OriginalGroundBackground{{0.0f,0.0f,0.0f,0.6f}};
std::atomic_bool BackgroundTintArmed{};
std::atomic_bool HideGroundArmed{};

using SharedLabelPaintFn = void(__fastcall*)(void*,void*,void*) noexcept;
SharedLabelPaintFn OriginalSharedLabelPaint{};
std::atomic_bool BackgroundPaintHookInstalled{};

namespace TooltipCompat = ::UnHoarder::TooltipCompatV1;
enum class TooltipCompatRoute : std::uint8_t { None, NativeOwner, ForeignHost };
std::atomic<TooltipCompatRoute> SharedLabelPaintCompatRoute{TooltipCompatRoute::None};
std::atomic<TooltipCompatRoute> GlyphRendererCompatRoute{TooltipCompatRoute::None};

struct TooltipPaintSubscriber final {
    TooltipCompat::RegistrationHandle handle{};
    TooltipCompat::SharedLabelPaintMiddlewareFn callback{};
    void* userData{};
    std::array<char,64> owner{};
};
struct TooltipGlyphSubscriber final {
    TooltipCompat::RegistrationHandle handle{};
    TooltipCompat::GlyphRendererMiddlewareFn callback{};
    void* userData{};
    std::array<char,64> owner{};
};

std::shared_ptr<const std::vector<TooltipPaintSubscriber>> TooltipPaintSubscribers{};
std::shared_ptr<const std::vector<TooltipGlyphSubscriber>> TooltipGlyphSubscribers{};
std::mutex TooltipCompatMutex;
std::uint64_t TooltipCompatNextHandle{1};
const D2RL::PluginCommunicationService* TooltipCompatCommunication{};
D2RL::PluginCommunication::ServiceLease<TooltipCompat::Service>
    ForeignPaintCompatLease{};
D2RL::PluginCommunication::ServiceLease<TooltipCompat::Service>
    ForeignGlyphCompatLease{};
TooltipCompat::RegistrationHandle ForeignPaintCompatHandle{};
TooltipCompat::RegistrationHandle ForeignGlyphCompatHandle{};

void __cdecl UnHoarderSharedLabelPaintMiddleware(
    void* userData,std::uintptr_t callerReturnAddress,
    void* rect,void* textArg,void* colorArg,
    TooltipCompat::SharedLabelPaintNextFn next,void* nextContext) noexcept;
std::uint64_t __cdecl UnHoarderGlyphRendererMiddleware(
    void* userData,std::uintptr_t callerReturnAddress,
    void* glyphContext,float x,float y,const float* rgba,
    TooltipCompat::GlyphRendererNextFn next,void* nextContext) noexcept;
void RunOwnedSharedLabelPaintChain(
    std::uintptr_t callerReturnAddress,void* rect,void* textArg,void* colorArg) noexcept;
std::uint64_t RunOwnedGlyphRendererChain(
    std::uintptr_t callerReturnAddress,void* glyphContext,
    float x,float y,const float* rgba) noexcept;

// Bounded ephemeral identity bridge between the formatter and painter. The
// formatter has the native item pointer while the painter has only the unit ID.
// No native pointers are retained, and stale identities expire after 3 seconds.
constexpr std::size_t IdentitySlots = 512;
constexpr std::size_t IdentityLookupWindow = 8;
constexpr ULONGLONG IdentityTtlMs = 3000;
struct VerifiedGroundIdentity {
    std::uint32_t unitId{};
    std::uint32_t code{};
    std::uint32_t classId{};
    ULONGLONG seenMs{};
    std::array<char,80> visibleName{};
    std::uint32_t quantity{};
    bool qualityKnown{};
    std::uint32_t quality{};
    bool itemLevelKnown{};
    std::uint32_t itemLevel{};
    bool socketsKnown{};
    std::uint32_t sockets{};
    bool etherealKnown{};
    bool ethereal{};
    bool identifiedKnown{};
    bool identified{};
};
std::array<VerifiedGroundIdentity,IdentitySlots> GroundIdentities{};
std::mutex GroundIdentityMutex;

void Emit(const char* message) noexcept;
void CodeText(std::uint32_t code, char (&out)[5]) noexcept;
bool PrintableItemCode(std::uint32_t code) noexcept;

constexpr std::int32_t GroundQuantityStatId=70;
constexpr std::uintptr_t GroundQuantityReaderRva=0x2F5020;
constexpr std::array<std::uint8_t,10> GroundQuantityBridgeLegacy{{
    0xFF,0x25,0x8A,0x51,0xB3,0x03,0x90,0x90,0x90,0x90}};
constexpr std::array<std::uint8_t,10> GroundQuantityBridgeUpdated{{
    0xFF,0x25,0x9A,0x51,0xB3,0x03,0x90,0x90,0x90,0x90}};
constexpr std::array<std::uint8_t,10> GroundQuantityBridgeSeptember{{
    0xFF,0x25,0xCA,0x51,0xB3,0x03,0x90,0x90,0x90,0x90}};
// Exact game-build-93847 / loader-1.3.1 RIP-indirect bridge captured
// 2026-09-24. Never accept arbitrary FF25 bridges: verify the slot AND owner.
constexpr std::array<std::uint8_t,10> GroundQuantityBridgeLoader131{{
    0xFF,0x25,0xF2,0x51,0xB3,0x03,0x90,0x90,0x90,0x90}};
using GroundQuantityReaderFn=std::int32_t(__fastcall*)(void*,std::int32_t,std::uint16_t) noexcept;
std::atomic<GroundQuantityReaderFn> GroundQuantityReader{};

// Initialized from GameJoined, never by an arbitrary renderer callback.
// The known loader bridge must terminate in D2RCore. Foreign executable targets
// fail closed rather than receiving a product-specific compatibility exception.
void QualifyGroundQuantityReader() noexcept {
    GroundQuantityReader.store(nullptr,std::memory_order_release);
    if (!Context || !Context->exeBase || !D2RL::GetBuildName(Context) ||
        std::string_view(D2RL::GetBuildName(Context))!="93847") return;
    const auto base=static_cast<std::uintptr_t>(Context->exeBase);
    std::array<std::uint8_t,10> entry{};
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(base+GroundQuantityReaderRva),
            entry.data(),entry.size(),&copied) || copied!=entry.size()) {
        Context->LogWarn("LOOT_QUANTITY_UNAVAILABLE reader-bridge-unreadable fallback=vanilla");
        return;
    }
    std::uintptr_t requiredSlot{};
    if (entry==GroundQuantityBridgeLegacy) requiredSlot=base+0x3E2A1B0;
    else if (entry==GroundQuantityBridgeUpdated) requiredSlot=base+0x3E2A1C0;
    else if (entry==GroundQuantityBridgeSeptember) requiredSlot=base+0x3E2A1F0;
    else if (entry==GroundQuantityBridgeLoader131) requiredSlot=base+0x3E2A218;
    else {
        // SDK/loader updates can change the loader-owned stat bridge while the
        // D2R build remains 93847. Report bytes; DO NOT trust or call it.
        std::array<std::uint8_t,24> window{};
        SIZE_T windowRead{};
        const bool windowOk=ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(base+GroundQuantityReaderRva),
            window.data(),window.size(),&windowRead) &&
            windowRead==window.size();
        char hex[24*3+1]{};
        for(std::size_t i=0;i<(windowOk?window.size():entry.size());++i)
            std::snprintf(hex+3*i,sizeof(hex)-3*i,"%02X ",
                unsigned(windowOk?window[i]:entry[i]));
        char line[310]{};
        std::snprintf(line,sizeof(line),
            "LOOT_COMPAT_QUANTITY version=" UNHOARDER_VERSION_STRING " build=93847 readerRva=0x%llX "
            "readOk=%u entryBytes=[%s] expected=known-loader-bridge "
            "newBridge=unqualified statCalls=disabled no-native-writes=1",
            static_cast<unsigned long long>(GroundQuantityReaderRva),
            windowOk?1U:0U,hex);
        Context->LogWarn(line);
        if(entry[0]==0xFF && entry[1]==0x25) {
            std::int32_t rel{};
            std::memcpy(&rel,entry.data()+2,sizeof(rel));
            const auto slot=static_cast<std::uintptr_t>(
                static_cast<std::int64_t>(GroundQuantityReaderRva+6)+rel);
            char slotLine[210]{};
            std::snprintf(slotLine,sizeof(slotLine),
                "LOOT_COMPAT_QUANTITY_BRIDGE kind=RIP-indirect slotRva=0x%llX "
                "pointerNotCalled=1 verify-new-owner-and-ABI=required",
                static_cast<unsigned long long>(slot));
            Context->LogInfo(slotLine);
        }
        Context->LogWarn("LOOT_QUANTITY_UNAVAILABLE reader-bridge-fingerprint-mismatch fallback=vanilla");
        return;
    }
    std::int32_t displacement{};
    std::memcpy(&displacement,entry.data()+2,sizeof(displacement));
    const auto actualSlot=static_cast<std::uintptr_t>(
        static_cast<std::int64_t>(base+GroundQuantityReaderRva+6)+displacement);
    if (actualSlot!=requiredSlot) return;
    const bool loader131Bridge=entry==GroundQuantityBridgeLoader131;
    if(loader131Bridge)
        Context->LogInfo("LOOT_COMPAT_QUANTITY_BRIDGE_ACCEPTED version=" UNHOARDER_VERSION_STRING " slotRva=0x3E2A218 fingerprint=exact targetAdmission=pending ownerRestriction=D2RCore-only");
    std::uintptr_t target{};
    copied=0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(actualSlot),&target,sizeof(target),&copied)
        || copied!=sizeof(target) || !target) return;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<const void*>(target),&memory,sizeof(memory))
        || memory.State!=MEM_COMMIT ||
        (memory.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return;
    const auto page=memory.Protect&0xFFU;
    if (page!=PAGE_EXECUTE && page!=PAGE_EXECUTE_READ &&
        page!=PAGE_EXECUTE_READWRITE && page!=PAGE_EXECUTE_WRITECOPY) return;
    HMODULE owner{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(target),&owner)) {
        if(loader131Bridge) Context->LogWarn("LOOT_COMPAT_QUANTITY_OWNER version=" UNHOARDER_VERSION_STRING " result=unresolved quantity=disabled");
        return;
    }
    if(loader131Bridge) {
        char ownerLine[220]{};
        const char* const ownerKind=owner==GetModuleHandleW(L"D2RCore.dll")?"D2RCore":"other";
        std::snprintf(ownerLine,sizeof(ownerLine),
            "LOOT_COMPAT_QUANTITY_OWNER version=" UNHOARDER_VERSION_STRING " owner=%s executePage=1 quantity=%s",
            ownerKind,std::string_view(ownerKind)=="other"?"disabled":"admitted");
        Context->LogInfo(ownerLine);
    }
    if (owner!=GetModuleHandleW(L"D2RCore.dll")) {
        Context->LogWarn("LOOT_QUANTITY_UNAVAILABLE reader-owner-unrecognized fallback=vanilla");
        return;
    }
    GroundQuantityReader.store(
        reinterpret_cast<GroundQuantityReaderFn>(base+GroundQuantityReaderRva),
        std::memory_order_release);
    Context->LogInfo("LOOT_QUANTITY_READY version=" UNHOARDER_VERSION_STRING " stat=quantity/70 source=qualified-loader-bridge mode=auto ground-only unitWrites=0");
}

std::uint32_t GroundStackQuantity(const void* borrowedNativeUnit) noexcept {
    const auto getter=GroundQuantityReader.load(std::memory_order_acquire);
    if (!getter || !borrowedNativeUnit) return 0;
    // Callers pass a synchronously qualified live native TYPE_ITEM pointer.
    const auto quantity=getter(const_cast<void*>(borrowedNativeUnit),
        GroundQuantityStatId,0);
    return quantity>1 && quantity<=65535 ?
        static_cast<std::uint32_t>(quantity) : 0;
}




bool GroundCandidatePageReadable(std::uintptr_t address,
    std::size_t bytes) noexcept {
    if(address<0x10000 || (address&7U)!=0 ||
        address>std::numeric_limits<std::uintptr_t>::max()-bytes)
        return false;
    MEMORY_BASIC_INFORMATION page{};
    if(VirtualQuery(reinterpret_cast<const void*>(address),&page,
            sizeof(page))!=sizeof(page) || page.State!=MEM_COMMIT ||
        (page.Protect&(PAGE_GUARD|PAGE_NOACCESS))!=0) return false;
    const auto readable=page.Protect&0xFFU;
    if(readable!=PAGE_READONLY && readable!=PAGE_READWRITE &&
       readable!=PAGE_WRITECOPY && readable!=PAGE_EXECUTE_READ &&
       readable!=PAGE_EXECUTE_READWRITE &&
       readable!=PAGE_EXECUTE_WRITECOPY) return false;
    const auto base=reinterpret_cast<std::uintptr_t>(page.BaseAddress);
    return address>=base && bytes<=page.RegionSize &&
        address-base<=page.RegionSize-bytes;
}


// Build 93847 production automap projection for JSON-configured item markers.
// These contracts were qualified during development and are retained with
// fingerprint validation; no capture buffers are used.
constexpr std::uintptr_t AutomapRenderUnitRva=0xD76E0;
constexpr std::uintptr_t ProjectClientToAutomapRva=0xD4910;
constexpr std::uintptr_t GetLocalDataContextRva=0x8B2D0;
constexpr std::uintptr_t GetLocalPlayerRva=0x9A480;
constexpr std::array<std::uint8_t,32> ExpectedAutomapRenderUnit{
    0x48,0x89,0x6C,0x24,0x10,0x57,0x48,0x83,
    0xEC,0x40,0x48,0x8B,0xFA,0x4C,0x8D,0x44,
    0x24,0x68,0x48,0x8D,0x54,0x24,0x60,0x48,
    0x8B,0xE9,0xE8,0xF1,0x01,0x00,0x00,0x84
};
constexpr std::array<std::uint8_t,32> ExpectedProjectClientToAutomap{
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,
    0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x57,
    0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20,
    0x66,0x0F,0x6E,0x41,0x10,0x4C,0x8B,0xFA
};
constexpr std::array<std::uint8_t,10> ExpectedGetLocalDataContext{
    0x8B,0x05,0x2E,0x84,0x99,0x02,0xC3,0xCC,0xCC,0xCC
};
constexpr std::array<std::uint8_t,32> ExpectedGetLocalPlayer{
    0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,
    0xEC,0x20,0x83,0xF9,0x08,0x0F,0x83,0x85,
    0x00,0x00,0x00,0x8B,0xD9,0x48,0x89,0x5C,
    0x24,0x38,0x48,0x83,0xFB,0x08,0x72,0x19
};
constexpr std::uintptr_t NativeUiStateTableRva=0x2A2ADA0;
constexpr std::uintptr_t NativeUiOpenStateWitnessRva=0x0CD7FB;
constexpr std::uintptr_t NativeUiCloseStateWitnessRva=0x0C7DF1;
constexpr std::uintptr_t NativeUiToggleStateWitnessRva=0x0CDE3C;
constexpr std::size_t NativeUiAutomapStateIndex=10;
constexpr std::array<std::uint8_t,9> ExpectedNativeUiOpenStateWitness{
    0x45,0x0F,0xB6,0xBC,0x1C,0xA0,0xAD,0xA2,0x02
};
constexpr std::array<std::uint8_t,19> ExpectedNativeUiCloseStateWitness{
    0x48,0x83,0xFB,0x20,0x0F,0x83,0x0C,0x04,0x00,0x00,
    0x42,0xC6,0x84,0x23,0xA0,0xAD,0xA2,0x02,0x00
};
constexpr std::array<std::uint8_t,9> ExpectedNativeUiToggleStateWitness{
    0x45,0x0F,0xB6,0xA4,0x1D,0xA0,0xAD,0xA2,0x02
};
constexpr std::size_t AutomapClipLeftOffset=0x18;
constexpr std::size_t AutomapClipTopOffset=0x1C;
constexpr std::size_t AutomapClipWidthOffset=0x20;
constexpr std::size_t AutomapClipHeightOffset=0x24;
constexpr std::uint64_t MinimapProjectionItemFreshMilliseconds=5000;
constexpr std::size_t MinimapProjectionItemCapacity=256;
constexpr std::size_t MinimapUnitBytes=0x40;
constexpr std::size_t MinimapPathBytes=0x28;

struct NativeAutomapPoint final { std::int32_t x{},y{}; };
using AutomapRenderUnitFn=void(__fastcall*)(void*,void*) noexcept;
using ProjectClientToAutomapFn=NativeAutomapPoint*(__fastcall*)(
    void*,NativeAutomapPoint*,std::uint64_t) noexcept;
using GetLocalDataContextFn=std::int32_t(__fastcall*)() noexcept;
using GetLocalPlayerFn=void*(__fastcall*)(std::int32_t) noexcept;

AutomapRenderUnitFn OriginalAutomapRenderUnit{};
ProjectClientToAutomapFn ProjectClientToAutomap{};
GetLocalDataContextFn GetLocalDataContext{};
GetLocalPlayerFn GetLocalPlayer{};
std::atomic_bool AutomapProjectionHookInstalled{};
std::atomic_bool AutomapProjectionArmed{};
std::atomic<std::uint64_t> AutomapMarkerFrameSequence{};

struct MinimapProjectionItem final {
    std::uint32_t code{},unitId{},classId{},mode{};
    std::int32_t worldX{},worldY{};
    std::uint64_t observedTick{};
    bool hasIcon{};
    MinimapOverlayRenderer::Shape shape{MinimapOverlayRenderer::Shape::Diamond};
    std::array<float,4> borderColor{};
    std::array<float,4> fillColor{};
    float sizePx{MinimapIconPolicy::DefaultSizePx};
};
std::mutex MinimapProjectionItemMutex;
std::array<MinimapProjectionItem,MinimapProjectionItemCapacity> MinimapProjectionItems{};
std::size_t MinimapProjectionItemCount{};

void ResetMinimapTracking() noexcept {
    AutomapProjectionArmed.store(false,std::memory_order_release);
    {
        std::lock_guard lock(MinimapProjectionItemMutex);
        MinimapProjectionItems.fill({});
        MinimapProjectionItemCount=0;
    }
    AutomapMarkerFrameSequence.store(0,std::memory_order_relaxed);
    MinimapOverlayRenderer::Clear();
    AutomapProjectionArmed.store(true,std::memory_order_release);
}

void UpdateMinimapProjectionPosition(std::uint32_t code,std::uint32_t unitId,
    std::uint32_t classId,std::uint32_t mode,std::uint32_t x,std::uint32_t y) noexcept {
    if(unitId==0) return;
    if(!MinimapProjectionItemMutex.try_lock()) return;
    const auto now=static_cast<std::uint64_t>(GetTickCount64());
    std::size_t slot=MinimapProjectionItemCount;
    for(std::size_t i=0;i<MinimapProjectionItemCount;++i) {
        if(MinimapProjectionItems[i].unitId==unitId &&
           MinimapProjectionItems[i].code==code) { slot=i;break; }
    }
    if(slot==MinimapProjectionItemCount) {
        if(MinimapProjectionItemCount<MinimapProjectionItemCapacity) {
            slot=MinimapProjectionItemCount++;
            MinimapProjectionItems[slot]={};
        } else {
            slot=0;
            for(std::size_t i=1;i<MinimapProjectionItemCount;++i)
                if(MinimapProjectionItems[i].observedTick<
                   MinimapProjectionItems[slot].observedTick) slot=i;
            MinimapProjectionItems[slot]={};
        }
    }
    auto& item=MinimapProjectionItems[slot];
    item.code=code;item.unitId=unitId;item.classId=classId;item.mode=mode;
    item.worldX=static_cast<std::int32_t>(x);
    item.worldY=static_cast<std::int32_t>(y);
    item.observedTick=now;
    MinimapProjectionItemMutex.unlock();
}

void UpdateMinimapProjectionIconRule(std::uint32_t unitId,std::uint32_t code,
    const GroundRuleDecision* rule) noexcept {
    if(unitId==0) return;
    if(!MinimapProjectionItemMutex.try_lock()) return;
    code=CanonicalItemCode(code);
    std::size_t slot=MinimapProjectionItemCount;
    for(std::size_t i=0;i<MinimapProjectionItemCount;++i) {
        if(MinimapProjectionItems[i].unitId==unitId &&
           MinimapProjectionItems[i].code==code) { slot=i;break; }
    }
    const bool hasIcon=rule!=nullptr && rule->hasMinimapIcon;
    if(slot==MinimapProjectionItemCount && hasIcon) {
        if(MinimapProjectionItemCount<MinimapProjectionItemCapacity) {
            slot=MinimapProjectionItemCount++;
            MinimapProjectionItems[slot]={};
        } else {
            slot=0;
            for(std::size_t i=1;i<MinimapProjectionItemCount;++i)
                if(MinimapProjectionItems[i].observedTick<
                   MinimapProjectionItems[slot].observedTick) slot=i;
            MinimapProjectionItems[slot]={};
        }
        MinimapProjectionItems[slot].unitId=unitId;
        MinimapProjectionItems[slot].code=code;
    }
    if(slot<MinimapProjectionItemCount) {
        auto& item=MinimapProjectionItems[slot];
        item.hasIcon=hasIcon;
        if(hasIcon) {
            item.shape=rule->minimapShape;
            item.borderColor=rule->minimapBorderColor;
            item.fillColor=rule->minimapFillColor;
            item.sizePx=rule->minimapSizePx;
        }
    }
    MinimapProjectionItemMutex.unlock();
}

void ClearMinimapProjectionIconStyles() noexcept {
    std::lock_guard lock(MinimapProjectionItemMutex);
    for(std::size_t i=0;i<MinimapProjectionItemCount;++i)
        MinimapProjectionItems[i].hasIcon=false;
    MinimapOverlayRenderer::Clear();
}

void ForgetMinimapProjectionItem(std::uint32_t unitId) noexcept {
    if(unitId==0 || !MinimapProjectionItemMutex.try_lock()) return;
    bool removed{};
    for(std::size_t i=0;i<MinimapProjectionItemCount;) {
        if(MinimapProjectionItems[i].unitId!=unitId) { ++i;continue; }
        removed=true;
        if(i+1<MinimapProjectionItemCount)
            MinimapProjectionItems[i]=MinimapProjectionItems[MinimapProjectionItemCount-1];
        MinimapProjectionItems[MinimapProjectionItemCount-1]={};
        --MinimapProjectionItemCount;
    }
    MinimapProjectionItemMutex.unlock();
    if(removed) MinimapOverlayRenderer::Clear();
}

bool IsLocalPlayerAutomapPass(void* unit) noexcept {
    __try {
        if(!unit || !GetLocalDataContext || !GetLocalPlayer) return false;
        const auto dataContext=GetLocalDataContext();
        if(dataContext<0 || dataContext>=8) return false;
        return GetLocalPlayer(dataContext)==unit;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

__declspec(noinline) void __fastcall HookAutomapRenderUnit(
        void* unit,void* automapContext) noexcept {
    const auto original=OriginalAutomapRenderUnit;
    if(!original) return;
    original(unit,automapContext);
    if(!AutomapProjectionArmed.load(std::memory_order_acquire) ||
       !automapContext || !IsLocalPlayerAutomapPass(unit)) return;
    const auto now=static_cast<std::uint64_t>(GetTickCount64());
    std::array<MinimapProjectionItem,MinimapProjectionItemCapacity> items{};
    std::size_t itemCount{};
    if(!MinimapProjectionItemMutex.try_lock()) return;
    for(std::size_t i=0;i<MinimapProjectionItemCount;++i) {
        const auto& item=MinimapProjectionItems[i];
        if(!item.hasIcon || item.observedTick==0 || now<item.observedTick ||
           now-item.observedTick>MinimapProjectionItemFreshMilliseconds) continue;
        items[itemCount++]=item;
    }
    MinimapProjectionItemMutex.unlock();
    if(!itemCount) { MinimapOverlayRenderer::Clear();return; }
    __try {
        const auto* contextBytes=static_cast<const std::uint8_t*>(automapContext);
        const AutomapProjection::ClipRect clip{
            *reinterpret_cast<const std::int32_t*>(contextBytes+AutomapClipLeftOffset),
            *reinterpret_cast<const std::int32_t*>(contextBytes+AutomapClipTopOffset),
            *reinterpret_cast<const std::int32_t*>(contextBytes+AutomapClipWidthOffset),
            *reinterpret_cast<const std::int32_t*>(contextBytes+AutomapClipHeightOffset)};
        if(!AutomapProjection::PlausibleClip(clip) || !ProjectClientToAutomap) {
            MinimapOverlayRenderer::Clear();return;
        }
        MinimapOverlayRenderer::MarkerFrame markerFrame{};
        markerFrame.clip={static_cast<float>(clip.left),static_cast<float>(clip.top),
            static_cast<float>(static_cast<std::int64_t>(clip.left)+clip.width),
            static_cast<float>(static_cast<std::int64_t>(clip.top)+clip.height)};
        markerFrame.publishedTick=now;
        markerFrame.sequence=AutomapMarkerFrameSequence.fetch_add(
            1,std::memory_order_relaxed)+1;
        for(std::size_t i=0;i<itemCount;++i) {
            const auto& item=items[i];
            AutomapProjection::Point client{};
            if(!AutomapProjection::WorldSubtileToClient(item.worldX,item.worldY,client))
                continue;
            NativeAutomapPoint projected{};
            if(ProjectClientToAutomap(automapContext,&projected,
                    AutomapProjection::PackClientCoordinates(client))!=&projected)
                continue;
            const AutomapProjection::Point screen{projected.x,projected.y};
            if(AutomapProjection::Contains(clip,screen) &&
               markerFrame.count<markerFrame.markers.size()) {
                auto& marker=markerFrame.markers[markerFrame.count++];
                marker.x=static_cast<float>(screen.x);
                marker.y=static_cast<float>(screen.y);
                marker.unitId=item.unitId;
                marker.shape=item.shape;
                marker.borderColor=item.borderColor;
                marker.fillColor=item.fillColor;
                marker.sizePx=item.sizePx;
            }
        }
        if(markerFrame.count) MinimapOverlayRenderer::Publish(markerFrame);
        else MinimapOverlayRenderer::Clear();
    } __except(EXCEPTION_EXECUTE_HANDLER) { MinimapOverlayRenderer::Clear(); }
}

bool InstallStandaloneAutomapProjection() noexcept {
    AutomapProjectionHookInstalled.store(false,std::memory_order_release);
    OriginalAutomapRenderUnit=nullptr;
    ProjectClientToAutomap=nullptr;
    GetLocalDataContext=nullptr;
    GetLocalPlayer=nullptr;
    if(!Context || !Base || !ImageSize) {
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=" UNHOARDER_VERSION_STRING " reason=no-image-or-context hooks=0 mapSenseDependency=0");
        return false;
    }
    if(GetModuleHandleW(L"d2rl-ruffneckk-mapsense.dll")!=nullptr) {
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=" UNHOARDER_VERSION_STRING " reason=mapsense-loaded-shared-rendezvous hooks=0 coexistence=preserved");
        return false;
    }
    const auto check=[&](std::uintptr_t rva,const auto& expected) {
        return Context->CheckExpectedBytes(rva,expected.data(),
            static_cast<std::uint32_t>(expected.size()));
    };
    if(!check(ProjectClientToAutomapRva,ExpectedProjectClientToAutomap) ||
       !check(GetLocalDataContextRva,ExpectedGetLocalDataContext) ||
       !check(GetLocalPlayerRva,ExpectedGetLocalPlayer) ||
       !check(AutomapRenderUnitRva,ExpectedAutomapRenderUnit)) {
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=" UNHOARDER_VERSION_STRING " reason=native-contract-fingerprint hooks=0");
        return false;
    }
    ProjectClientToAutomap=reinterpret_cast<ProjectClientToAutomapFn>(Base+ProjectClientToAutomapRva);
    GetLocalDataContext=reinterpret_cast<GetLocalDataContextFn>(Base+GetLocalDataContextRva);
    GetLocalPlayer=reinterpret_cast<GetLocalPlayerFn>(Base+GetLocalPlayerRva);
    if(!Context->InstallInlineHook(AutomapRenderUnitRva,ExpectedAutomapRenderUnit.data(),
            static_cast<std::uint32_t>(ExpectedAutomapRenderUnit.size()),
            HookAutomapRenderUnit,&OriginalAutomapRenderUnit) || !OriginalAutomapRenderUnit) {
        ProjectClientToAutomap=nullptr;GetLocalDataContext=nullptr;GetLocalPlayer=nullptr;
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=" UNHOARDER_VERSION_STRING " reason=loader-hook-registration hooks=0");
        return false;
    }
    AutomapProjectionHookInstalled.store(true,std::memory_order_release);
    Emit("LOOT_MINIMAP_PROJECTION_READY version=" UNHOARDER_VERSION_STRING " hook=D2R+0xD76E0 project=D2R+0xD4910 worldPosition=qualified-static-path renderer=standalone mapSenseDependency=0");
    return true;
}

void __cdecl LogMinimapRendererDiagnostic(const char* message) noexcept { Emit(message); }

bool ConfigureNativeAutomapVisibilityGate() noexcept {
    MinimapOverlayRenderer::SetAutomapVisibilityTable(nullptr);
    if(!Context || !Base) {
        Emit("LOOT_MINIMAP_AUTOMAP_GATE_REFUSED version=" UNHOARDER_VERSION_STRING " reason=no-image-or-context drawing=0");
        return false;
    }
    const auto check=[&](std::uintptr_t rva,const auto& expected) {
        return Context->CheckExpectedBytes(rva,expected.data(),
            static_cast<std::uint32_t>(expected.size()));
    };
    if(!check(NativeUiOpenStateWitnessRva,ExpectedNativeUiOpenStateWitness) ||
       !check(NativeUiCloseStateWitnessRva,ExpectedNativeUiCloseStateWitness) ||
       !check(NativeUiToggleStateWitnessRva,ExpectedNativeUiToggleStateWitness)) {
        Emit("LOOT_MINIMAP_AUTOMAP_GATE_REFUSED version=" UNHOARDER_VERSION_STRING " reason=ui-state-fingerprint drawing=0");
        return false;
    }
    MinimapOverlayRenderer::SetAutomapVisibilityTable(
        reinterpret_cast<const volatile std::uint8_t*>(Base+NativeUiStateTableRva));
    Emit("LOOT_MINIMAP_AUTOMAP_GATE_READY version=" UNHOARDER_VERSION_STRING " table=D2R+0x2A2ADA0 automapState=10 readOnly=1");
    return true;
}

bool InitializeMinimapMarkerRenderer() noexcept {
    HMODULE self{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&InitializeMinimapMarkerRenderer),&self) || !self) {
        Emit("LOOT_MINIMAP_RENDERER_REFUSED version=" UNHOARDER_VERSION_STRING " backend=none reason=self-module-unresolved");
        return false;
    }
    if(!ConfigureNativeAutomapVisibilityGate()) return false;
    MinimapOverlayRenderer::SetDllModule(self);
    MinimapOverlayRenderer::SetLogCallback(&LogMinimapRendererDiagnostic);
    return MinimapOverlayRenderer::Initialize();
}

void ObserveMinimapItemPosition(const void* nativeUnit,std::uint32_t rawCode,
    std::uint32_t unitId,std::uint32_t classId) noexcept {
    if(!AutomapProjectionArmed.load(std::memory_order_acquire) || !nativeUnit ||
       !unitId || !PrintableItemCode(rawCode)) return;
    std::array<std::uint8_t,MinimapUnitBytes> first{},last{};
    std::array<std::uint8_t,MinimapPathBytes> pathFirst{},pathLast{};
    SIZE_T copied{};
    if(!ReadProcessMemory(GetCurrentProcess(),nativeUnit,first.data(),first.size(),&copied) ||
       copied!=first.size()) return;
    std::uint32_t type{},nativeClass{},nativeId{},mode{};
    std::memcpy(&type,first.data(),4);std::memcpy(&nativeClass,first.data()+4,4);
    std::memcpy(&nativeId,first.data()+8,4);std::memcpy(&mode,first.data()+12,4);
    if(type!=4 || nativeClass!=classId || nativeId!=unitId || (mode!=3 && mode!=5)) return;
    std::uintptr_t pathAddress{};
    std::memcpy(&pathAddress,first.data()+0x38,sizeof(pathAddress));
    if(!pathAddress || !GroundCandidatePageReadable(pathAddress,MinimapPathBytes)) return;
    copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(pathAddress),
            pathFirst.data(),pathFirst.size(),&copied) || copied!=pathFirst.size()) return;
    copied=0;
    const bool pathAgain=ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(pathAddress),pathLast.data(),pathLast.size(),&copied) &&
        copied==pathLast.size();
    copied=0;
    const bool unitAgain=ReadProcessMemory(GetCurrentProcess(),nativeUnit,last.data(),
        last.size(),&copied) && copied==last.size();
    if(!pathAgain || !unitAgain || pathFirst!=pathLast ||
       std::memcmp(first.data(),last.data(),0x10)!=0 ||
       std::memcmp(first.data()+0x38,last.data()+0x38,sizeof(pathAddress))!=0) return;
    const auto coordinates=MinimapWorldPosition::Decode(pathFirst.data(),pathFirst.size());
    if(!coordinates.available || !coordinates.plausible) return;
    UpdateMinimapProjectionPosition(CanonicalItemCode(rawCode),unitId,classId,mode,
        coordinates.x,coordinates.y);
}

// Build 93847 native ground reader promoted after exact id+code post-pickup
// SDK comparisons (quality 3/6/4, ilvl 84/84/99). Read-only; no handles are
// forged from a native pointer; only the qualified ethereal bit is decoded
// when the first potentially matching rule requests it.
// Every active read uses the qualified bounded one-hop native item-data source.
// One unreadable field invalidates ALL property-dependent rules, including
// later generic hide fallbacks, rather than treating missing values as zero.
std::atomic<std::uint64_t> GroundPropertyLiveReads{};
std::atomic<std::uint64_t> GroundPropertyLiveUnknown{};
std::atomic<std::uint64_t> GroundEtherealRuleReads{};
std::atomic<std::uint64_t> GroundEtherealRuleUnknown{};
std::atomic<std::uint64_t> GroundEtherealMode5Reads{};
std::atomic<std::uint64_t> GroundIdentifiedRuleReads{};
std::atomic<std::uint64_t> GroundIdentifiedRuleUnknown{};
std::atomic<std::uint64_t> GroundIdentifiedMode5Reads{};
// Only a verified ground-label presentation path may qualify mode 5.
// Pickup/action suppression remains strictly mode 3 at its own guard.
GroundPropertyLive::Scalars ReadNativeGroundQualityLevel(
    const void* nativeUnit,std::uint32_t expectedId,
    std::uint32_t expectedClassId,
    GroundPropertyLive::Purpose purpose,
    const char** reason=nullptr,
    bool includeEthereal=false,
    bool includeIdentified=false) noexcept {
    const auto fail=[reason](const char* why) noexcept
        -> GroundPropertyLive::Scalars {
        if(reason)*reason=why;
        return {};
    };
    if(reason)*reason="ok";
    if (!nativeUnit || !expectedId || !Context ||
        !D2RL::GetBuildName(Context) ||
        std::string_view(D2RL::GetBuildName(Context))!="93847")
        return fail("unit-or-build-unavailable");
    std::array<std::uint8_t,0x18> before{},after{};
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(),nativeUnit,before.data(),
            before.size(),&copied) || copied!=before.size())
        return fail("unit-header-read-failed");
    const auto type=GroundPropertyReader::ReadLe32(before.data());
    const auto classId=GroundPropertyReader::ReadLe32(before.data()+4);
    const auto id=GroundPropertyReader::ReadLe32(before.data()+8);
    const auto mode=GroundPropertyReader::ReadLe32(before.data()+12);
    if(type!=4 || id!=expectedId || classId!=expectedClassId ||
       !GroundPropertyLive::AllowsMode(mode,purpose))
        return fail("ground-identity-mismatch");
    std::uintptr_t address{};
    std::memcpy(&address,before.data()+0x10,sizeof(address));
    constexpr std::size_t length=0x40;
    if(!GroundCandidatePageReadable(address,length))
        return fail("item-data-page-not-readable");
    std::array<std::uint8_t,length> itemData{},check{};
    copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(address),itemData.data(),
            itemData.size(),&copied) || copied!=itemData.size())
        return fail("item-data-first-read-failed");
    copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),nativeUnit,after.data(),
            after.size(),&copied) || copied!=after.size() ||
       before!=after) return fail("unit-changed-during-read");
    copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(address),check.data(),
            check.size(),&copied) || copied!=check.size())
        return fail("item-data-second-read-failed");
    if(GroundPropertyReader::ReadLe32(itemData.data())!=
           GroundPropertyReader::ReadLe32(check.data()) ||
       GroundPropertyReader::ReadLe32(itemData.data()+0x38)!=
           GroundPropertyReader::ReadLe32(check.data()+0x38))
        return fail("quality-or-level-changed-during-read");
    // Flag snapshot is required only when a potentially matching flag rule
    // requests it. Quality/ilvl-only rules avoid the flag gate entirely.
    if((includeEthereal || includeIdentified) &&
       GroundPropertyReader::ReadLe32(itemData.data()+0x18)!=
           GroundPropertyReader::ReadLe32(check.data()+0x18))
        return fail("item-flags-changed-during-read");
    const auto quality=GroundPropertyReader::ReadLe32(itemData.data());
    const auto level=GroundPropertyReader::ReadLe32(itemData.data()+0x38);
    auto scalars=GroundPropertyLive::Validate(quality,level);
    if(includeEthereal && scalars.qualityKnown && scalars.itemLevelKnown) {
        scalars.etherealKnown=true;
        scalars.ethereal=GroundEthereal::FromNativeFlags(
            GroundPropertyReader::ReadLe32(itemData.data()+0x18));
        if(mode==GroundPropertyLive::PresentingMode)
            GroundEtherealMode5Reads.fetch_add(1,std::memory_order_relaxed);
    }
    if(includeIdentified && scalars.qualityKnown && scalars.itemLevelKnown) {
        scalars.identifiedKnown=true;
        scalars.identified=GroundIdentified::FromNativeFlags(
            GroundPropertyReader::ReadLe32(itemData.data()+0x18));
        if(mode==GroundPropertyLive::PresentingMode)
            GroundIdentifiedMode5Reads.fetch_add(1,std::memory_order_relaxed);
    }
    if((!scalars.qualityKnown || !scalars.itemLevelKnown) && reason)
        *reason="quality-or-level-out-of-range";
    else if(reason && mode==GroundPropertyLive::PresentingMode)
        *reason="ok-mode5-verified-label";
    return scalars;
}


constexpr std::int32_t GroundSocketStatId=194;
std::atomic<std::uint64_t> GroundSocketRuleReads{};
std::atomic<std::uint64_t> GroundSocketRuleUnknown{};
std::atomic<std::uint64_t> GroundSocketMode5Reads{};
bool ReadNativeGroundSockets(const void* nativeUnit,
    std::uint32_t expectedId,std::uint32_t expectedClassId,
    GroundPropertyLive::Purpose purpose,std::uint32_t& sockets) noexcept {
    sockets=0; // meaningful ONLY when the return value is true
    if(!nativeUnit || !expectedId || !Context ||
       !D2RL::GetBuildName(Context) ||
       std::string_view(D2RL::GetBuildName(Context))!="93847") return false;
    const auto getter=GroundQuantityReader.load(std::memory_order_acquire);
    if(!getter) return false; // never interpret missing bridge as zero sockets
    std::array<std::uint32_t,4> before{},after{};
    SIZE_T copied{};
    if(!ReadProcessMemory(GetCurrentProcess(),nativeUnit,before.data(),
        sizeof(before),&copied) || copied!=sizeof(before) ||
       before[0]!=4 || before[1]!=expectedClassId ||
       before[2]!=expectedId ||
       !GroundPropertyLive::AllowsMode(before[3],purpose)) return false;
    const auto first=getter(const_cast<void*>(nativeUnit),
        GroundSocketStatId,0);
    copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),nativeUnit,after.data(),
        sizeof(after),&copied) || copied!=sizeof(after) ||
       before!=after) return false;
    // Two matching scalar reads protect against a transient stat result.
    const auto second=getter(const_cast<void*>(nativeUnit),
        GroundSocketStatId,0);
    copied=0;
    std::array<std::uint32_t,4> finalHeader{};
    if(!ReadProcessMemory(GetCurrentProcess(),nativeUnit,finalHeader.data(),
        sizeof(finalHeader),&copied) || copied!=sizeof(finalHeader) ||
       finalHeader!=before || first!=second || first<0 || first>15)
        return false;
    sockets=static_cast<std::uint32_t>(first);
    if(before[3]==GroundPropertyLive::PresentingMode)
        GroundSocketMode5Reads.fetch_add(1,std::memory_order_relaxed);
    return true;
}

RuleEngine::Item GroundRuleItem(std::uint32_t code,const void* nativeUnit,
    const FilterRuleTable* table,std::uint32_t expectedId=0,
    GroundPropertyLive::Purpose purpose=
        GroundPropertyLive::Purpose::StrictGround) noexcept {
    RuleEngine::Item item{};
    item.code=CanonicalItemCode(code);
    if (!nativeUnit) return item;

    std::array<std::uint32_t,4> header{};
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(),nativeUnit,header.data(),
            sizeof(header),&copied) || copied!=sizeof(header) ||
        header[0]!=4 || !GroundPropertyLive::AllowsMode(header[3],purpose) ||
        !header[2] || (expectedId && header[2]!=expectedId))
        return item;

    item.classIdKnown=true;
    item.classId=header[1];
    if (table && table->usesQuantity &&
        GroundQuantityReader.load(std::memory_order_acquire)) {
        const auto quantity=GroundStackQuantity(nativeUnit);
        item.quantityKnown=true;
        item.quantity=quantity>1?quantity:1;
    }

    // Read only the first native property needed by the current ordered rule,
    // then re-evaluate. Unknown native values stop property-dependent matching
    // so a later broad Hide cannot turn an unreadable item into a false match.
    for(unsigned propertyGroup=0;propertyGroup<4 && table;++propertyGroup) {
        const auto next=RuleEngine::NextNativeProperty(table->rules,item);
        if(next==RuleEngine::NextProperty::None) break;

        if(next==RuleEngine::NextProperty::Ethereal) {
            GroundEtherealRuleReads.fetch_add(1,std::memory_order_relaxed);
            const auto fields=ReadNativeGroundQualityLevel(nativeUnit,
                header[2],header[1],purpose,nullptr,true);
            if(!fields.etherealKnown) {
                GroundEtherealRuleUnknown.fetch_add(1,std::memory_order_relaxed);
                break;
            }
            item.etherealKnown=true;
            item.ethereal=fields.ethereal;
            item.qualityKnown=fields.qualityKnown;
            item.quality=fields.quality;
            item.itemLevelKnown=fields.itemLevelKnown;
            item.itemLevel=fields.itemLevel;
            continue;
        }

        if(next==RuleEngine::NextProperty::Identified) {
            GroundIdentifiedRuleReads.fetch_add(1,std::memory_order_relaxed);
            const auto fields=ReadNativeGroundQualityLevel(nativeUnit,
                header[2],header[1],purpose,nullptr,false,true);
            if(!fields.identifiedKnown) {
                GroundIdentifiedRuleUnknown.fetch_add(1,std::memory_order_relaxed);
                break;
            }
            item.identifiedKnown=true;
            item.identified=fields.identified;
            item.qualityKnown=fields.qualityKnown;
            item.quality=fields.quality;
            item.itemLevelKnown=fields.itemLevelKnown;
            item.itemLevel=fields.itemLevel;
            continue;
        }

        if(next==RuleEngine::NextProperty::Sockets) {
            GroundSocketRuleReads.fetch_add(1,std::memory_order_relaxed);
            std::uint32_t count{};
            const bool known=ReadNativeGroundSockets(nativeUnit,header[2],
                header[1],purpose,count);
            if(!known) GroundSocketRuleUnknown.fetch_add(1,std::memory_order_relaxed);
            item.socketsKnown=known;
            if(known) item.sockets=count;
            if(!known) break;
            continue;
        }

        if((table->usesQuality || table->usesItemLevel) &&
           RuleEngine::NeedsNativeQualityLevel(table->rules,item)) {
            GroundPropertyLiveReads.fetch_add(1,std::memory_order_relaxed);
            const auto fields=ReadNativeGroundQualityLevel(
                nativeUnit,header[2],header[1],purpose);
            if((table->usesQuality && !fields.qualityKnown) ||
               (table->usesItemLevel && !fields.itemLevelKnown))
                GroundPropertyLiveUnknown.fetch_add(1,std::memory_order_relaxed);
            item.qualityKnown=fields.qualityKnown;
            item.quality=fields.quality;
            item.itemLevelKnown=fields.itemLevelKnown;
            item.itemLevel=fields.itemLevel;
            if(!fields.qualityKnown || !fields.itemLevelKnown) break;
        }
    }
    return item;
}

RuleEngine::Item CachedGroundRuleItem(std::uint32_t code,
    std::uint32_t classId,std::uint32_t quantity,
    const FilterRuleTable* table,
    const RuleEngine::Item* scalarSnapshot=nullptr) noexcept {
    RuleEngine::Item item{};item.code=CanonicalItemCode(code);
    item.classIdKnown=true;item.classId=classId;
    if(table && table->usesQuantity &&
       GroundQuantityReader.load(std::memory_order_acquire)) {
        item.quantityKnown=true;item.quantity=quantity>1?quantity:1;
    }
    if(table && scalarSnapshot) {
        if(table->usesQuality) {
            item.qualityKnown=scalarSnapshot->qualityKnown;
            item.quality=scalarSnapshot->quality;
        }
        if(table->usesItemLevel) {
            item.itemLevelKnown=scalarSnapshot->itemLevelKnown;
            item.itemLevel=scalarSnapshot->itemLevel;
        }
        if(table->usesSockets) {
            item.socketsKnown=scalarSnapshot->socketsKnown;
            item.sockets=scalarSnapshot->sockets;
        }
        if(table->usesEthereal) {
            item.etherealKnown=scalarSnapshot->etherealKnown;
            item.ethereal=scalarSnapshot->ethereal;
        }
        if(table->usesIdentified) {
            item.identifiedKnown=scalarSnapshot->identifiedKnown;
            item.identified=scalarSnapshot->identified;
        }
    }
    return item;
}
void MergeGroundRuleAction(GroundRuleDecision& output,
    const FilterNameRule& rule) noexcept {
    // Every matched block updates visibility, exactly like PoE Show/Hide.
    output.show=rule.show;
    if(rule.hasName) {
        output.hasName=true;output.name=rule.name;output.bytes=rule.bytes;
    }
    if(rule.hasBackground) {
        output.hasBackground=true;output.background=rule.background;
    }
    if(rule.hasTextColor) {
        output.hasTextColor=true;output.textColor=rule.textColor;
    }
    if(rule.hasDropSound) {
        output.hasDropSound=true;output.dropSound=rule.dropSound;
    }
    if(rule.hasMinimapIcon) {
        output.hasMinimapIcon=true;output.minimapShape=rule.minimapShape;
        output.minimapBorderColor=rule.minimapBorderColor;
        output.minimapFillColor=rule.minimapFillColor;
        output.minimapSizePx=rule.minimapSizePx;
    }
}

bool ResolveGroundRule(const FilterRuleTable* table,
    const RuleEngine::Item& item,GroundRuleDecision& output) noexcept {
    output={};
    if(!table) return false;
    return RuleEngine::ResolveMatchingRulesFailOpen(table->rules,item,
        [&](const FilterNameRule& rule) noexcept {
            MergeGroundRuleAction(output,rule);
        },[&]() noexcept {
            // Visibility is fail-open while a later potentially-matching
            // Show/Hide block still depends on an unavailable native value.
            // Already-composed non-visibility actions may remain staged.
            output.show=true;
        });
}

// UnHoarder is standalone. Game lifecycle is used only to reset per-game
// state and refresh optional native readers; core filtering does not depend on
// Sanctuary of Exile or any mod-specific callback/export contract.
const D2RL::LifecycleService* InWorldLifecycle{};
D2RL::Lifecycle::ListenerHandle InWorldJoinedListener{D2RL::Lifecycle::InvalidHandle};

void RuntimeWorkerStart() noexcept;
void RuntimeWorkerStop() noexcept;
bool ReloadFilterRules();
bool ActivateConfiguredFilter(bool automatic) noexcept;
void RebaselineSoundAtGameJoin() noexcept;

void __cdecl OnInWorldGameJoined(const D2RL::PluginContext*,
    const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
    if (!event || event->kind != D2RL::Lifecycle::GameplayEventKind::GameJoined)
        return;
    ResetMinimapTracking();
    if (AutomapProjectionHookInstalled.load(std::memory_order_acquire) &&
        std::string_view(MinimapOverlayRenderer::ActiveBackendName())=="none")
        (void)InitializeMinimapMarkerRenderer();
    QualifyGroundQuantityReader();
    if (!std::atomic_load_explicit(&PublishedFilterRules,
            std::memory_order_acquire) && !FilterConfigPath.empty() &&
        ReloadFilterRules())
        (void)ActivateConfiguredFilter(true);
    RebaselineSoundAtGameJoin();
}

void RegisterInWorldLifecycle() noexcept {
    if (!Context) return;
    const D2RL::LifecycleService* service{};
    if (Context->QueryService(&service) != D2RL::ServiceQueryResult::Success ||
        !D2RL::HasLifecycleServiceField(service,
            D2RL::LifecycleServiceRequiredSize) ||
        !service->registerGameplayEventListener) {
        Emit("LOOT_GAME_LIFECYCLE_UNAVAILABLE per-game-refresh-disabled=1");
        return;
    }
    const D2RL::Lifecycle::GameplayEventListener listener{
        D2RL::Lifecycle::GameplayEventListenerSize, 0,
        D2RL::Lifecycle::GameplayEventKind::GameJoined, 0,
        &OnInWorldGameJoined, nullptr
    };
    if (service->registerGameplayEventListener(Context, &listener,
            &InWorldJoinedListener) == D2RL::Lifecycle::Result::Success &&
        InWorldJoinedListener != D2RL::Lifecycle::InvalidHandle) {
        InWorldLifecycle = service;
        Emit("LOOT_GAME_LIFECYCLE_READY per-game-refresh=1");
    } else {
        Emit("LOOT_GAME_LIFECYCLE_REFUSED per-game-refresh-disabled=1");
    }
}

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .abiVersion = D2RL_PLUGIN_ABI_VERSION,
    .id = "loot-filter",
    .name = "UnHoarder",
    .version = UNHOARDER_VERSION_STRING,
    .author = "MindH1ve",
    .description = "Live-reloadable JSON loot filter with Show/Hide rules, tooltip styling, sounds and automap icons (D2R 93847).",
    .flags = D2RL::PluginFlags::Client |
             D2RL::PluginFlags::NativeHooks |
             D2RL::PluginFlags::ModScopedOnly,
};

// Native hooks do not write files or emit verbose sampling logs.
void Emit(const char* message) noexcept {
    if (!Context || !message) return;

    // Production logging only: startup, refusal, compatibility, and reload state.
    const bool warning =
        std::strstr(message,"LOOT_RULES_REFUSED") ||
        std::strstr(message,"LOOT_FILTER_REFUSED") ||
        std::strstr(message,"LOOT_FILTER_INACTIVE") ||
        std::strstr(message,"LOOT_FILTER_AUTO_INACTIVE") ||
        std::strstr(message,"LOOT_GLYPH_B_REFUSED") ||
        std::strstr(message,"LOOT_PICKUP_GUARD_REFUSED") ||
        std::strstr(message,"LOOT_PICKUP_GUARD_INACTIVE") ||
        std::strstr(message,"LOOT_NATIVE_ACTION_REFUSED") ||
        std::strstr(message,"LOOT_NATIVE_ACTION_PARTIAL") ||
        std::strstr(message,"LOOT_NATIVE_ACTION_UNAVAILABLE") ||
        std::strstr(message,"LOOT_GAME_LIFECYCLE_UNAVAILABLE") ||
        std::strstr(message,"LOOT_GAME_LIFECYCLE_REFUSED") ||
        std::strstr(message,"LOOT_SOUND_REFUSED") ||
        std::strstr(message,"LOOT_SOUND_QUALIFY refused=") ||
        std::strstr(message,"LOOT_MINIMAP_RENDERER_REFUSED") ||
        std::strstr(message,"LOOT_TOOLTIP_COMPAT_REFUSED");
    if (warning) {
        Context->LogWarn(message);
        return;
    }

    const bool operational =
        std::strstr(message,"LOOT_FILTER_READY") ||
        std::strstr(message,"LOOT_RULES_LOADED") ||
        std::strstr(message,"LOOT_RULES_PATH") ||
        std::strstr(message,"LOOT_CONFIG_LEGACY_PATH") ||
        std::strstr(message,"LOOT_FILTER_AUTO_ACTIVE") ||
        std::strstr(message,"LOOT_RELOAD_") ||
        std::strstr(message,"LOOT_PICKUP_GUARD_READY") ||
        std::strstr(message,"LOOT_SOUND_QUALIFY matched=") ||
        std::strstr(message,"LOOT_SOUND_ARMED") ||
        std::strstr(message,"LOOT_MINIMAP_PROJECTION_READY") ||
        std::strstr(message,"LOOT_MINIMAP_AUTOMAP_GATE_READY") ||
        std::strstr(message,"LOOT_MINIMAP_RENDERER_READY") ||
        std::strstr(message,"LOOT_TOOLTIP_COMPAT_READY") ||
        std::strstr(message,"LOOT_TOOLTIP_COMPAT_CHAINED");
    if (operational) Context->LogInfo(message);
}

void CodeText(std::uint32_t code, char (&out)[5]) noexcept {
    for (int i = 0; i < 4; ++i) {
        const auto ch = static_cast<unsigned char>(code >> (i * 8));
        out[i] = ch == 0 ? ' ' : (ch >= 32 && ch <= 126 ? static_cast<char>(ch) : '.');
    }
    out[4] = '\0';
}

bool MatchesCode(std::uint32_t code, std::string_view selected) noexcept {
    if (selected.empty()) return true;
    char chars[5]{};
    CodeText(code, chars);
    std::string_view compact(chars, 4);
    while (!compact.empty() && compact.back() == ' ') compact.remove_suffix(1);
    return selected == compact;
}

// Still no native item reads: the original qualified helper does all item access.
std::uint32_t __fastcall HookGetItemCode(void* item) noexcept {
    return OriginalGetItemCode(item);
}

bool DetermineImageSize() noexcept {
    if (!Base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    ImageSize = nt->OptionalHeader.SizeOfImage;
    return ImageSize >= 0x100000 && ImageSize <= 0x40000000;
}

bool ReadSafe(std::uintptr_t rva, void* output, std::size_t count) noexcept {
    if (!Base || !ImageSize || rva >= ImageSize || count > ImageSize - rva) return false;
    const auto address = Base + rva;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        address + count > reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize)
        return false;
    std::memcpy(output, reinterpret_cast<const void*>(address), count);
    return true;
}

// Item-code validation used by production rule evaluation and native guards.
bool PrintableItemCode(std::uint32_t code) noexcept {
    const auto compact = CanonicalItemCode(code);
    if (compact == 0) return false;
    bool padded = false;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        const auto ch = static_cast<unsigned char>(compact >> shift);
        if (ch == 0) { padded = true; continue; }
        if (padded || ch < 0x20 || ch > 0x7e) return false;
    }
    return true;
}


// Resolve the canonical filter.json path, retaining read-only migration
// Fallbacks for previous production/development filenames when the canonical file is absent.
bool ResolveFilterConfigPath() noexcept {
    HMODULE self{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ResolveFilterConfigPath), &self) || !self)
        return false;
    std::array<wchar_t, 32768> modulePath{};
    const auto length = GetModuleFileNameW(self, modulePath.data(),
        static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) return false;

    const auto directory=std::filesystem::path(modulePath.data()).parent_path();
    const auto canonical=directory/L"filter.json";
    const auto previousProduction=directory/L"loot-filter.json";
    const auto legacyDevelopmentConfig=directory/L"loot-filter-probe.json";
    std::error_code error;
    if(std::filesystem::exists(canonical,error) && !error) {
        FilterConfigPath=canonical;
        return true;
    }
    error.clear();
    if(std::filesystem::exists(previousProduction,error) && !error) {
        FilterConfigPath=previousProduction;
        Emit("LOOT_CONFIG_LEGACY_PATH using=loot-filter.json rename-to=filter.json");
        return true;
    }
    error.clear();
    if(std::filesystem::exists(legacyDevelopmentConfig,error) && !error) {
        FilterConfigPath=legacyDevelopmentConfig;
        Emit("LOOT_CONFIG_LEGACY_PATH using=loot-filter-probe.json rename-to=filter.json");
        return true;
    }
    FilterConfigPath=canonical;
    return true;
}

std::string PathUtf8(const std::filesystem::path& path) {
    if (path.empty()) return "<unavailable>";
    const auto wide = path.wstring();
    const int required = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "<conversion-failed>";
    std::string utf8(static_cast<std::size_t>(required), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
        utf8.data(), required, nullptr, nullptr)) return "<conversion-failed>";
    utf8.pop_back();
    return utf8;
}

std::string FilterPathUtf8() { return PathUtf8(FilterConfigPath); }

// Poll only from the background runtime worker. No file metadata or parser
// work happens in item-code, formatter, painter, hover, sound, or pickup hooks.
FilterLiveReload::Stamp ReadFilterFileStamp(
    const std::filesystem::path& path) noexcept {
    FilterLiveReload::Stamp result{};
    if (path.empty()) return result;
    std::error_code error;
    const auto length=std::filesystem::file_size(path,error);
    if (error) return result; // missing/unreadable: one refused reload on transition
    const auto modified=std::filesystem::last_write_time(path,error);
    if (error) return result;
    result.present=true;
    result.bytes=static_cast<std::uint64_t>(length);
    result.modified=modified;
    return result;
}

std::uint32_t PackFilterCode(std::string_view code) noexcept {
    std::uint32_t packed{};
    for (std::size_t i = 0; i < code.size() && i < 4; ++i)
        packed |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(code[i])) << (8U * i);
    return CanonicalItemCode(packed);
}

// Keep the name -> base-code catalog entirely within a single reload.
// A failed table read never changes the published rules or starts a partially
// resolved filter. Do not load tables from native item or painter callbacks.
struct BaseNameResolution {
    BaseNameTable::Catalog catalog{};
    std::filesystem::path excel{};
    bool loaded{};
};
bool EnsureBaseNameCatalog(BaseNameResolution& names,std::string& error) {
    if(names.loaded) return true;
    names.excel=BaseNameTable::DiscoverExcel(FilterConfigPath);
    if(names.excel.empty()) {
        error="baseName-requires-weapons.txt-and-armor.txt-excel-tables";
        return false;
    }
    if(!BaseNameTable::LoadPair(names.excel,names.catalog,error)) return false;
    names.loaded=true;
    return true;
}

// Reload-scoped ItemTypes hierarchy and item table catalog. The selectors
// compile to concrete canonical item codes, so mode-5 label/sound matching
// needs no new native read, stat bridge or per-drop I/O.
struct ItemTypeResolution {
    ItemTypeTable::Catalog catalog{};
    std::filesystem::path excel{};
    bool loaded{};
};
bool EnsureItemTypeCatalog(ItemTypeResolution& types,std::string& error) {
    if(types.loaded)return true;
    types.excel=BaseNameTable::DiscoverExcel(FilterConfigPath);
    if(types.excel.empty()) {
        error="itemType-requires-mod-excel-weapons-armor-misc-itemtypes";
        return false;
    }
    if(!ItemTypeTable::Load(types.excel,types.catalog,error))return false;
    types.loaded=true;
    return true;
}

// Version 2 conditions are strictly validated. An Excel 'name' is resolved
// once at reload into base item codes; never into class IDs / localized labels.
bool ParseV2Conditions(const nlohmann::json& value,
    RuleEngine::Conditions& dest,BaseNameResolution& names,
    ItemTypeResolution& types,std::string& error) {
    if (!value.is_object() || value.empty()) {
        error="conditions-must-be-nonempty-object";return false;
    }
    const auto natural=[](const nlohmann::json& v,std::uint32_t& n) {
        if (!v.is_number_integer()) return false;
        if (v.is_number_unsigned()) {
            const auto u=v.get<std::uint64_t>();
            if (u>UINT32_MAX) return false;
            n=static_cast<std::uint32_t>(u);return true;
        }
        const auto i=v.get<std::int64_t>();
        if (i<0 || i>UINT32_MAX) return false;
        n=static_cast<std::uint32_t>(i);return true;
    };
    for(auto it=value.begin();it!=value.end();++it) {
        const auto& key=it.key();
        if(key=="code" || key=="baseName") {
            const auto& values=it.value();
            if(!values.is_string() && !values.is_array()) {
                error=key+"-requires-name-or-array";return false;
            }
            std::vector<std::uint32_t> parsed;
            const auto one=[&](const nlohmann::json& part) -> bool {
                if(!part.is_string()) return false;
                const auto text=part.get<std::string>();
                if(key=="baseName") {
                    if(text.empty() || text.size()>127 ||
                       text.find('\t')!=text.npos ||
                       text.find('\r')!=text.npos ||
                       text.find('\n')!=text.npos) return false;
                    if(!EnsureBaseNameCatalog(names,error)) return false;
                    const auto* codes=names.catalog.Find(text);
                    if(!codes) {
                        error="unknown-baseName-in-weapons-or-armor:"+
                            text.substr(0,75);
                        return false;
                    }
                    parsed.insert(parsed.end(),codes->begin(),codes->end());
                    return true;
                }
                if(text.empty() || text.size()>4 ||
                   !std::all_of(text.begin(),text.end(),[](unsigned char ch) {
                       return ch>=0x20 && ch<=0x7e;
                   })) return false;
                const auto packed=PackFilterCode(text);
                if(!PrintableItemCode(packed))return false;
                parsed.push_back(packed);return true;
            };
            if(values.is_array()) {
                if(values.empty() || values.size()>64) {
                    error="condition-array-must-have-1..64-values";return false;
                }
                for(const auto& v:values) if(!one(v)) {
                    if(error.empty()) error="invalid-"+key+"-value";
                    return false;
                }
            } else if(!one(values)) {
                if(error.empty()) error="invalid-"+key+"-value";
                return false;
            }
            std::sort(parsed.begin(),parsed.end());
            parsed.erase(std::unique(parsed.begin(),parsed.end()),parsed.end());
            if(key=="code") dest.codes=std::move(parsed);
            else dest.baseCodes=std::move(parsed);
        } else if(key=="itemType") {
            // Exact ItemTypes.Code (e.g. swor, axe, tors, shld) OR literal
            // ItemTypes.ItemType name. Equiv1/Equiv2 are expanded transitively
            // over both direct item type and type2 at atomic JSON reload.
            const auto& values=it.value();
            if(!values.is_string() && !values.is_array()) {
                error="itemType-requires-name-code-or-array";return false;
            }
            std::vector<std::uint32_t> parsed;
            const auto one=[&](const nlohmann::json& part)->bool {
                if(!part.is_string())return false;
                const auto token=part.get<std::string>();
                if(token.empty() || token.size()>127 ||
                   token.find('\t')!=token.npos ||
                   token.find('\r')!=token.npos ||
                   token.find('\n')!=token.npos)return false;
                if(!EnsureItemTypeCatalog(types,error))return false;
                const auto* codes=types.catalog.Find(token);
                if(!codes) {
                    error="unknown-itemType-in-itemtypes.txt:"+token.substr(0,75);
                    return false;
                }
                parsed.insert(parsed.end(),codes->begin(),codes->end());
                return true;
            };
            if(values.is_array()) {
                if(values.empty() || values.size()>64) {
                    error="itemType-array-must-have-1..64-values";return false;
                }
                for(const auto& v:values)if(!one(v)) {
                    if(error.empty())error="invalid-itemType-value";
                    return false;
                }
            } else if(!one(values)) {
                if(error.empty())error="invalid-itemType-value";
                return false;
            }
            std::sort(parsed.begin(),parsed.end());
            parsed.erase(std::unique(parsed.begin(),parsed.end()),parsed.end());
            if(parsed.empty()) {
                error="itemType-selector-has-no-item-codes";return false;
            }
            dest.typeCodes=std::move(parsed);
        } else if(key=="rarity") {
            // Text labels align with D2R ItemInfo numeric quality enum.
            // OR within array; AND across fields, as for code/baseName.
            const auto qualityValue=[](const nlohmann::json& v,
                    std::uint32_t& out) -> bool {
                if(!v.is_string()) return false;
                const auto label=v.get<std::string>();
                constexpr std::array<std::pair<std::string_view,
                    std::uint32_t>,9> choices{{
                    {"inferior",1},{"normal",2},{"superior",3},
                    {"magic",4},{"set",5},{"rare",6},
                    {"unique",7},{"crafted",8},{"tempered",9}
                }};
                for(const auto& [name,value]:choices)
                    if(label==name) {out=value;return true;}
                return false;
            };
            const auto& values=it.value();
            if(!values.is_string() && !values.is_array()) {
                error="rarity-requires-name-or-names";return false;
            }
            std::vector<std::uint32_t> parsed;
            if(values.is_array()) {
                if(values.empty() || values.size()>9) {
                    error="rarity-array-must-have-1..9-names";return false;
                }
                for(const auto& value:values) {
                    std::uint32_t number{};
                    if(!qualityValue(value,number)) {
                        error="unsupported-rarity-name";return false;
                    }
                    parsed.push_back(number);
                }
            } else {
                std::uint32_t number{};
                if(!qualityValue(values,number)) {
                    error="unsupported-rarity-name";return false;
                }
                parsed.push_back(number);
            }
            std::sort(parsed.begin(),parsed.end());
            parsed.erase(std::unique(parsed.begin(),parsed.end()),parsed.end());
            dest.qualities=std::move(parsed);
        } else if(key=="identified") {
            if(!it.value().is_boolean()) {
                error="identified-requires-boolean-true-or-false";
                return false;
            }
            dest.identifiedEnabled=true;
            dest.identifiedExpected=it.value().get<bool>();
        } else if(key=="ethereal") {
            if(!it.value().is_boolean()) {
                error="ethereal-requires-boolean-true-or-false";
                return false;
            }
            dest.etherealEnabled=true;
            dest.etherealExpected=it.value().get<bool>();
        } else if(key=="itemLevel" || key=="quantity" || key=="sockets") {
            const bool level=key=="itemLevel";
            const bool socket=key=="sockets";
            if(!it.value().is_object() || it.value().empty()) {
                error=key+"-requires-comparison-object";return false;
            }
            auto& numberTest=level?dest.itemLevel:(socket?dest.sockets:dest.quantity);
            numberTest.enabled=true;
            for(auto q=it.value().begin();q!=it.value().end();++q) {
                std::uint32_t number{};
                if(!natural(q.value(),number) ||
                   (level ? (number<1 || number>99) :
                    (socket ? number>15U : number>65535U))) {
                    error=level?"itemLevel-requires-integer-1..99":
                        (socket?"sockets-requires-integer-0..15":
                            "quantity-requires-integer-0..65535");return false;
                }
                if(q.key()=="eq") {
                    if(numberTest.hasEq) return false;
                    numberTest.hasEq=true;numberTest.eq=number;
                } else if(q.key()=="gt" || q.key()=="gte") {
                    if(numberTest.hasMin) {
                        error=key+"-duplicate-min-bound";return false;
                    }
                    numberTest.hasMin=true;numberTest.min=number;
                    numberTest.minInclusive=q.key()=="gte";
                } else if(q.key()=="lt" || q.key()=="lte") {
                    if(numberTest.hasMax) {
                        error=key+"-duplicate-max-bound";return false;
                    }
                    numberTest.hasMax=true;numberTest.max=number;
                    numberTest.maxInclusive=q.key()=="lte";
                } else {
                    error="unsupported-"+key+"-operator:"+q.key();return false;
                }
            }
        } else {
            error="unsupported-ground-condition:"+key;
            return false;
        }
    }
    return true;
}

// RGBA is a JSON STRING: "RGBA(255, 0, 128, 0.82)". Integer RGB channels
// use 0..255, alpha is a locale-independent decimal 0..1. No clamping or
// silent fallback on invalid input; the previous complete ruleset survives.
bool ParseFilterRgba(std::string_view value, std::array<float,4>& result) noexcept {
    if (value.size()<10 || !value.starts_with("RGBA(") || value.back()!=')')
        return false;
    value.remove_prefix(5);
    value.remove_suffix(1);
    const auto trim=[](std::string_view input) noexcept {
        while (!input.empty() && (input.front()==' ' || input.front()=='\t')) input.remove_prefix(1);
        while (!input.empty() && (input.back()==' ' || input.back()=='\t')) input.remove_suffix(1);
        return input;
    };
    std::array<std::string_view,4> parts{};
    for (std::size_t i=0;i<4;i++) {
        const auto comma=value.find(',');
        if (i<3 && comma==std::string_view::npos) return false;
        if (i==3 && comma!=std::string_view::npos) return false;
        parts[i]=trim(i<3?value.substr(0,comma):value);
        if (parts[i].empty()) return false;
        if (i<3) value.remove_prefix(comma+1);
    }
    for (std::size_t i=0;i<3;i++) {
        unsigned int channel{};
        const auto [end,error]=std::from_chars(parts[i].data(),
            parts[i].data()+parts[i].size(),channel,10);
        if (error!=std::errc{} || end!=parts[i].data()+parts[i].size() || channel>255)
            return false;
        result[i]=static_cast<float>(channel)/255.0f;
    }
    float alpha{};
    const auto [end,error]=std::from_chars(parts[3].data(),
        parts[3].data()+parts[3].size(),alpha,std::chars_format::general);
    if (error!=std::errc{} || end!=parts[3].data()+parts[3].size() ||
        !std::isfinite(alpha) || alpha<0.0f || alpha>1.0f) return false;
    result[3]=alpha;
    return true;
}

// Console/lifecycle only: full file validation before atomic publication.
// Invalid edits NEVER replace the last successfully loaded snapshot.
bool ReloadFilterRules() {
    if (FilterConfigPath.empty()) {
        Emit("LOOT_RULES_REFUSED path-unavailable");
        return false;
    }
    try {
        std::error_code error;
        const auto length = std::filesystem::file_size(FilterConfigPath, error);
        if (error || length == 0 || length > MaximumFilterFileBytes) {
            Emit("LOOT_RULES_REFUSED config-missing-empty-or-over-4MiB last-valid-rules-preserved=1");
            return false;
        }
        std::ifstream file(FilterConfigPath, std::ios::binary);
        if (!file) {
            Emit("LOOT_RULES_REFUSED config-open-failed last-valid-rules-preserved=1");
            return false;
        }
        auto config = nlohmann::json::parse(file);
        if (!config.is_object() || !config.contains("version") ||
            !config["version"].is_number_integer() ||
            (config["version"].get<int>() != 1 &&
             config["version"].get<int>() != 2 &&
             config["version"].get<int>() != 3) ||
            !config.contains("rules") || !config["rules"].is_array()) {
            Emit("LOOT_RULES_REFUSED required-schema={version:1|2|3,rules:array} last-valid-rules-preserved=1");
            return false;
        }
        const auto& entries = config["rules"];
        if (entries.size() > MaximumFilterRules) {
            Emit("LOOT_RULES_REFUSED over-4096-rules last-valid-rules-preserved=1");
            return false;
        }
        auto fresh = std::make_shared<FilterRuleTable>();
        fresh->schema=static_cast<std::uint32_t>(config["version"].get<int>());
        fresh->rules.reserve(entries.size());
        std::unordered_set<std::uint32_t> seen;
        BaseNameResolution baseNames{};
        ItemTypeResolution itemTypes{};
        std::size_t line = 0;
        for (const auto& entry : entries) {
            ++line;
            if(!entry.is_object()) {
                char message[220]{};
                std::snprintf(message,sizeof(message),
                    "LOOT_RULES_REFUSED entry=%zu rule-must-be-object last-valid-rules-preserved=1",line);
                Emit(message);return false;
            }

            // Schema 3 mirrors PoE block structure: each ordered array entry
            // is exactly one {"show":{...}} or {"hide":{...}} block.
            // The wrapper itself is the visibility action. Schema 1/2 remain
            // accepted as migration formats and retain first-match semantics.
            const nlohmann::json* block=&entry;
            bool wrapperShow{};
            if(fresh->schema==3) {
                const bool hasShow=entry.contains("show");
                const bool hasHide=entry.contains("hide");
                if(entry.size()!=1U || hasShow==hasHide ||
                   (hasShow && !entry["show"].is_object()) ||
                   (hasHide && !entry["hide"].is_object())) {
                    char message[300]{};
                    std::snprintf(message,sizeof(message),
                        "LOOT_RULES_REFUSED entry=%zu schema3-requires-exactly-one-wrapper={show:object}|{hide:object} last-valid-rules-preserved=1",line);
                    Emit(message);return false;
                }
                wrapperShow=hasShow;
                block=&entry[hasShow?"show":"hide"];
            }

            if((fresh->schema==1 && (!block->contains("code") ||
                 !(*block)["code"].is_string())) ||
               (fresh->schema==2 && (!block->contains("conditions") ||
                 !(*block)["conditions"].is_object() ||
                 block->contains("code"))) ||
               (fresh->schema==3 &&
                 ((block->contains("conditions") && !(*block)["conditions"].is_object()) ||
                  block->contains("code") || block->contains("show") ||
                  block->contains("hide"))) ||
               (block->contains("ruleName") && !(*block)["ruleName"].is_string()) ||
               (block->contains("name") && !(*block)["name"].is_string()) ||
               (block->contains("tooltip") && !(*block)["tooltip"].is_object()) ||
               (block->contains("backgroundColor") && !(*block)["backgroundColor"].is_string()) ||
               (block->contains("textColor") && !(*block)["textColor"].is_string()) ||
               (block->contains("dropSound") && !(*block)["dropSound"].is_string()) ||
               (block->contains("minimapIcon") && !(*block)["minimapIcon"].is_object()) ||
               (fresh->schema<=2 && block->contains("show") && !(*block)["show"].is_boolean()) ||
               (fresh->schema==3 && block->contains("continue") && !(*block)["continue"].is_boolean()) ||
               (fresh->schema<=2 && !block->contains("show") &&
                 !block->contains("name") && !block->contains("tooltip") &&
                 !block->contains("backgroundColor") && !block->contains("textColor") &&
                 !block->contains("dropSound") && !block->contains("minimapIcon"))) {
                char message[340]{};
                std::snprintf(message,sizeof(message),
                    fresh->schema==3 ?
                    "LOOT_RULES_REFUSED entry=%zu invalid-show-hide-block fields={ruleName?,conditions?,continue?,name?,tooltip?,dropSound?,minimapIcon?} last-valid-rules-preserved=1" :
                    "LOOT_RULES_REFUSED entry=%zu requires-code/conditions-and-action(show|name|tooltip|dropSound|minimapIcon) last-valid-rules-preserved=1",line);
                Emit(message);return false;
            }

            // v3 is intentionally strict: colors belong only inside tooltip.
            // v1/v2 keep the legacy flat aliases for migration.
            if(fresh->schema==3 &&
               (block->contains("backgroundColor") || block->contains("textColor"))) {
                char message[280]{};
                std::snprintf(message,sizeof(message),
                    "LOOT_RULES_REFUSED entry=%zu schema3-flat-colors-unsupported use-tooltip-object last-valid-rules-preserved=1",line);
                Emit(message);return false;
            }
            if (block->contains("tooltip")) {
                const auto& tooltip=(*block)["tooltip"];
                if (block->contains("backgroundColor") || block->contains("textColor")) {
                    char message[260]{};
                    std::snprintf(message,sizeof(message),
                        "LOOT_RULES_REFUSED entry=%zu tooltip-cannot-be-mixed-with-flat-backgroundColor-or-textColor last-valid-rules-preserved=1",line);
                    Emit(message);return false;
                }
                if (tooltip.empty() || tooltip.size()>2U ||
                    (tooltip.contains("backgroundColor") && !tooltip["backgroundColor"].is_string()) ||
                    (tooltip.contains("textColor") && !tooltip["textColor"].is_string()) ||
                    (!tooltip.contains("backgroundColor") && !tooltip.contains("textColor"))) {
                    char message[300]{};
                    std::snprintf(message,sizeof(message),
                        "LOOT_RULES_REFUSED entry=%zu invalid-tooltip expected={backgroundColor?:RGBA(...),textColor?:RGBA(...)} at-least-one-required last-valid-rules-preserved=1",line);
                    Emit(message);return false;
                }
                for (auto key=tooltip.begin();key!=tooltip.end();++key) {
                    if (key.key()!="backgroundColor" && key.key()!="textColor") {
                        Emit("LOOT_RULES_REFUSED invalid-tooltip unexpected-field last-valid-rules-preserved=1");
                        return false;
                    }
                }
            }

            if(fresh->schema==2) {
                for(auto key=block->begin();key!=block->end();++key) {
                    if(key.key()!="conditions" && key.key()!="show" &&
                       key.key()!="name" && key.key()!="tooltip" &&
                       key.key()!="textColor" && key.key()!="backgroundColor" &&
                       key.key()!="dropSound" && key.key()!="minimapIcon") {
                        Emit("LOOT_RULES_REFUSED unexpected-v2-action-or-field last-valid-rules-preserved=1");
                        return false;
                    }
                }
            } else if(fresh->schema==3) {
                for(auto key=block->begin();key!=block->end();++key) {
                    if(key.key()!="ruleName" && key.key()!="conditions" && key.key()!="continue" &&
                       key.key()!="name" && key.key()!="tooltip" &&
                       key.key()!="dropSound" && key.key()!="minimapIcon") {
                        Emit("LOOT_RULES_REFUSED unexpected-v3-block-field last-valid-rules-preserved=1");
                        return false;
                    }
                }
            }

            const auto code = fresh->schema==1 ?
                (*block)["code"].get<std::string>() : std::string{};
            const bool hasName=block->contains("name");
            const auto name=hasName?(*block)["name"].get<std::string>():std::string{};
            if (fresh->schema==1 &&
                (code.empty() || code.size() > 4 ||
                !std::all_of(code.begin(),code.end(),[](unsigned char ch){
                    return ch >= 0x20 && ch <= 0x7e;
                }) || !PrintableItemCode(PackFilterCode(code)))) {
                char message[200]{};
                std::snprintf(message,sizeof(message),
                    "LOOT_RULES_REFUSED entry=%zu code-must-be-1..4-printable-ASCII-bytes-with-nonspace-content last-valid-rules-preserved=1",line);
                Emit(message);return false;
            }
            std::size_t lines = 1;
            std::size_t currentLine = 0;
            bool safeName = !hasName || (!name.empty() && name.size() <= MaximumFilterNameBytes);
            for (const unsigned char ch : name) {
                if (ch == '\n') {
                    ++lines;
                    if (currentLine == 0) safeName = false;
                    currentLine = 0;
                } else if (ch >= 0x20 && ch <= 0x7e) {
                    if (++currentLine > 55) safeName = false;
                } else safeName = false;
            }
            if (hasName && (currentLine == 0 || lines > 3)) safeName = false;
            if (!safeName) {
                char message[240]{};
                std::snprintf(message,sizeof(message),
                    "LOOT_RULES_REFUSED entry=%zu name-must-be-1..79-ASCII-bytes-max-3-nonempty-lines-55-chars-per-line last-valid-rules-preserved=1",line);
                Emit(message);return false;
            }
            const auto codeValue = PackFilterCode(code);
            if (fresh->schema==1 && !seen.insert(codeValue).second) {
                char message[200]{};
                std::snprintf(message,sizeof(message),
                    "LOOT_RULES_REFUSED entry=%zu duplicate-code last-valid-rules-preserved=1",line);
                Emit(message);return false;
            }

            FilterNameRule rule{};
            rule.schema2=fresh->schema>=2;
            rule.code=codeValue;
            rule.show=fresh->schema==3 ? wrapperShow :
                (!block->contains("show") || (*block)["show"].get<bool>());
            rule.continueEvaluation=fresh->schema==3 && block->contains("continue") &&
                (*block)["continue"].get<bool>();
            if(!rule.show) ++fresh->hiddenRules;

            if(rule.schema2) {
                std::string failure;
                if(block->contains("conditions")) {
                    if(!ParseV2Conditions((*block)["conditions"],rule.conditions,
                           baseNames,itemTypes,failure)) {
                        char message[300]{};
                        std::snprintf(message,sizeof(message),
                            "LOOT_RULES_REFUSED entry=%zu %s last-valid-rules-preserved=1",
                            line,failure.c_str());
                        Emit(message);return false;
                    }
                }
                if (rule.conditions.quantity.enabled) fresh->usesQuantity=true;
                if (!rule.conditions.qualities.empty()) fresh->usesQuality=true;
                if (rule.conditions.itemLevel.enabled) fresh->usesItemLevel=true;
                if (rule.conditions.sockets.enabled) fresh->usesSockets=true;
                if (rule.conditions.etherealEnabled) fresh->usesEthereal=true;
                if (rule.conditions.identifiedEnabled) fresh->usesIdentified=true;
                if (!rule.conditions.typeCodes.empty()) fresh->usesItemType=true;
            }

            if (hasName) {
                std::memcpy(rule.name.data(),name.data(),name.size());
                rule.name[name.size()]='\0';
                rule.bytes=name.size()+1;
                rule.hasName=true;
            }
            const auto* tooltip = block->contains("tooltip")
                ? &(*block)["tooltip"] : nullptr;
            const auto* backgroundColor = tooltip && tooltip->contains("backgroundColor")
                ? &(*tooltip)["backgroundColor"]
                : (fresh->schema<=2 && block->contains("backgroundColor") ?
                    &(*block)["backgroundColor"] : nullptr);
            const auto* textColor = tooltip && tooltip->contains("textColor")
                ? &(*tooltip)["textColor"]
                : (fresh->schema<=2 && block->contains("textColor") ?
                    &(*block)["textColor"] : nullptr);
            if (backgroundColor != nullptr) {
                const auto color=backgroundColor->get<std::string>();
                if (!ParseFilterRgba(color,rule.background)) {
                    char message[320]{};
                    std::snprintf(message,sizeof(message),
                        "LOOT_RULES_REFUSED entry=%zu invalid-tooltip-backgroundColor expected=RGBA(r,g,b,a) r/g/b=0..255 a=0..1 last-valid-rules-preserved=1",line);
                    Emit(message);return false;
                }
                rule.hasBackground=true;++fresh->backgroundRules;
            }
            if (textColor != nullptr) {
                const auto color=textColor->get<std::string>();
                if (!ParseFilterRgba(color,rule.textColor)) {
                    char message[320]{};
                    std::snprintf(message,sizeof(message),
                        "LOOT_RULES_REFUSED entry=%zu invalid-tooltip-textColor expected=RGBA(r,g,b,a) r/g/b=0..255 a=0..1 last-valid-rules-preserved=1",line);
                    Emit(message);return false;
                }
                rule.hasTextColor=true;++fresh->textColorRules;
            }
            if (block->contains("dropSound")) {
                const auto sound=(*block)["dropSound"].get<std::string>();
                const auto validSound=[](unsigned char c) noexcept {
                    return (c>='A' && c<='Z') || (c>='a' && c<='z') ||
                           (c>='0' && c<='9') || c=='_' || c=='-';
                };
                if (sound.empty() || sound.size() >= rule.dropSound.size() ||
                    !std::all_of(sound.begin(),sound.end(),validSound)) {
                    char message[280]{};
                    std::snprintf(message,sizeof(message),
                        "LOOT_RULES_REFUSED entry=%zu invalid-dropSound expected=1..63-sounds.txt-row-name-ASCII last-valid-rules-preserved=1",line);
                    Emit(message);return false;
                }
                std::memcpy(rule.dropSound.data(),sound.c_str(),sound.size()+1);
                rule.hasDropSound=true;++fresh->soundRules;
            }
            if (block->contains("minimapIcon")) {
                const auto& icon=(*block)["minimapIcon"];
                const bool hasSize=icon.contains("size");
                if((icon.size()!=3U && icon.size()!=4U) ||
                   !icon.contains("shape") || !icon.contains("borderColor") ||
                   !icon.contains("fillColor") ||
                   !icon["shape"].is_string() ||
                   !icon["borderColor"].is_string() ||
                   !icon["fillColor"].is_string() ||
                   (hasSize && !icon["size"].is_number_integer())) {
                    char message[360]{};
                    std::snprintf(message,sizeof(message),
                        "LOOT_RULES_REFUSED entry=%zu invalid-minimapIcon required={shape:string,borderColor:RGBA(...),fillColor:RGBA(...),size?:integer-px} last-valid-rules-preserved=1",line);
                    Emit(message);return false;
                }
                for(auto key=icon.begin();key!=icon.end();++key) {
                    if(key.key()!="shape" && key.key()!="borderColor" &&
                       key.key()!="fillColor" && key.key()!="size") {
                        Emit("LOOT_RULES_REFUSED invalid-minimapIcon unexpected-field last-valid-rules-preserved=1");
                        return false;
                    }
                }
                const auto shape=icon["shape"].get<std::string>();
                if(shape=="circle") rule.minimapShape=MinimapOverlayRenderer::Shape::Circle;
                else if(shape=="diamond") rule.minimapShape=MinimapOverlayRenderer::Shape::Diamond;
                else if(shape=="triangle") rule.minimapShape=MinimapOverlayRenderer::Shape::Triangle;
                else if(shape=="star") rule.minimapShape=MinimapOverlayRenderer::Shape::Star;
                else {
                    char message[260]{};
                    std::snprintf(message,sizeof(message),
                        "LOOT_RULES_REFUSED entry=%zu invalid-minimapIcon-shape expected=circle|diamond|triangle|star last-valid-rules-preserved=1",line);
                    Emit(message);return false;
                }
                if(!ParseFilterRgba(icon["borderColor"].get<std::string>(),
                       rule.minimapBorderColor) ||
                   !ParseFilterRgba(icon["fillColor"].get<std::string>(),
                       rule.minimapFillColor)) {
                    char message[300]{};
                    std::snprintf(message,sizeof(message),
                        "LOOT_RULES_REFUSED entry=%zu invalid-minimapIcon-color expected=RGBA(r,g,b,a) r/g/b=0..255 a=0..1 last-valid-rules-preserved=1",line);
                    Emit(message);return false;
                }
                if(hasSize) {
                    const auto requestedSize=icon["size"].get<std::int64_t>();
                    if(!MinimapIconPolicy::TryNormalizeSizePx(
                            requestedSize,rule.minimapSizePx)) {
                        char message[300]{};
                        std::snprintf(message,sizeof(message),
                            "LOOT_RULES_REFUSED entry=%zu invalid-minimapIcon-size expected=integer-px clamped-to-12..40 last-valid-rules-preserved=1",line);
                        Emit(message);return false;
                    }
                }
                rule.hasMinimapIcon=true;++fresh->minimapIconRules;
            }
            fresh->rules.push_back(rule);
        }
        // Resolution is complete before the atomic publish. A read failure,
        // unknown name, or ambiguous source leaves the previous filter active.
        if(baseNames.loaded) {
            fresh->baseNamesExcelPath=baseNames.excel;
            char message[250]{};
            std::snprintf(message,sizeof(message),
                "LOOT_BASENAME_TABLES_READY weapons=%zu armor=%zu distinctNames=%zu source=weapons.txt+armor.txt mode=literal-name-to-code",
                baseNames.catalog.weaponRows,baseNames.catalog.armorRows,
                baseNames.catalog.byName.size());
            Emit(message);
            const auto path=std::string("LOOT_BASENAME_TABLES_PATH '")+
                PathUtf8(baseNames.excel)+"'";
            Emit(path.c_str());
        }
        if(itemTypes.loaded) {
            fresh->itemTypesExcelPath=itemTypes.excel;
            char message[320]{};
            std::snprintf(message,sizeof(message),
                "LOOT_ITEMTYPE_TABLES_READY types=%zu weapons=%zu armor=%zu misc=%zu resolvedSelectors=%zu source=itemtypes.txt+weapons.txt+armor.txt+misc.txt hierarchy=Equiv1+Equiv2 type2=1 mode=reload-code-expansion",
                itemTypes.catalog.typeRows,itemTypes.catalog.weaponRows,
                itemTypes.catalog.armorRows,itemTypes.catalog.miscRows,
                itemTypes.catalog.itemCodesByType.size());
            Emit(message);
            const auto path=std::string("LOOT_ITEMTYPE_TABLES_PATH '")+
                PathUtf8(itemTypes.excel)+"'";
            Emit(path.c_str());
        }
        fresh->generation = FilterGeneration.fetch_add(1,std::memory_order_acq_rel)+1;
        const std::shared_ptr<const FilterRuleTable> published=fresh;
        std::atomic_store_explicit(&PublishedFilterRules,published,
            std::memory_order_release);
        ClearMinimapProjectionIconStyles();
        char message[560]{};
        std::snprintf(message,sizeof(message),
            "LOOT_RULES_LOADED version=" UNHOARDER_VERSION_STRING " schema=%u generation=%llu rules=%zu backgroundRules=%zu textColorRules=%zu soundRules=%zu minimapIconRules=%zu hiddenRules=%zu syntax=schema3:{show|hide:{ruleName?,conditions?,continue?,name?,tooltip?,dropSound?,minimapIcon?}} tooltip{backgroundColor,textColor}+RGBA(r,g,b,a) minimapShapes=circle|diamond|triangle|star minimapSizePx=default12,clamped12..40 maxNameBytes=%zu propertyQuality=%u propertyIlvl=%u propertySockets=%u propertyEthereal=%u propertyIdentified=%u propertyItemType=%u reload=atomic nativeItemWrites=0",
            fresh->schema,static_cast<unsigned long long>(fresh->generation),
            fresh->rules.size(),fresh->backgroundRules,fresh->textColorRules,
            fresh->soundRules,fresh->minimapIconRules,fresh->hiddenRules,MaximumFilterNameBytes,
            fresh->usesQuality?1U:0U,fresh->usesItemLevel?1U:0U,
            fresh->usesSockets?1U:0U,fresh->usesEthereal?1U:0U,
            fresh->usesIdentified?1U:0U,fresh->usesItemType?1U:0U);
        Emit(message);
        return true;
    } catch (const std::exception& e) {
        const std::string detail = std::string("LOOT_RULES_REFUSED invalid-json-or-config: ") +
            e.what() + " last-valid-rules-preserved=1";
        Emit(detail.substr(0,350).c_str());
        return false;
    } catch (...) {
        Emit("LOOT_RULES_REFUSED parse-allocation-failure last-valid-rules-preserved=1");
        return false;
    }
}

// Read/write a *specific* 93847 ground-name output region only. The native
// inner writer is invoked first; our substitution happens while its parent
// formatter has NOT YET measured text or calculated x/y/width/height.
// Verified call site: 0x1FAA18, return address 0x1FAA1D. The rdx pointer is
// record+0x24, while its text starts at record+0x28. r8d=0x80.
std::uint64_t __fastcall HookInnerNameWriter(
    void* unit, void* output, std::uint32_t bufferSize, void* outMetadata) noexcept {
    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto result = OriginalInnerNameWriter(unit, output, bufferSize, outMetadata);
    GeometryTotalCalls.fetch_add(1, std::memory_order_relaxed);
    const auto mode = ActiveGeometryMode.load(std::memory_order_acquire);
    if (mode != GeometryMode::Rules ||
        returnAddress != Base + InnerNameWriterCallerRva + DirectCallBytes ||
        !unit || !output || bufferSize != InnerNameBufferBytes ||
        !OriginalGetItemCode || !HookInstalled.load(std::memory_order_acquire))
        return result;
    GeometrySourceCalls.fetch_add(1, std::memory_order_relaxed);
    const auto outputAddress = reinterpret_cast<std::uintptr_t>(output);
    if (outputAddress < 0x24 || outputAddress > UINTPTR_MAX - 4) {
        GeometryReadFailures.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const auto recordAddress = outputAddress - 0x24;
    std::uint32_t header[4]{};
    std::uint32_t recordId{};
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(), unit, header, sizeof(header), &copied) ||
        copied != sizeof(header) || header[0] != 4) {
        GeometryReadFailures.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    copied=0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(recordAddress + 0x10),
            &recordId, sizeof(recordId), &copied) || copied != sizeof(recordId)) {
        GeometryReadFailures.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    if (recordId != header[2]) {
        GeometryIdMismatches.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    GeometryValidPairs.fetch_add(1, std::memory_order_relaxed);
    GeometryLookups.fetch_add(1, std::memory_order_relaxed);
    const auto code = CanonicalItemCode(OriginalGetItemCode(unit)); // original helper trampoline
    ObserveMinimapItemPosition(unit,code,recordId,header[1]);
    const char* configuredName = nullptr;
    std::size_t configuredBytes = 0;
    std::shared_ptr<const FilterRuleTable> snapshot;
    {
        FilterLookups.fetch_add(1,std::memory_order_relaxed);
        if (!PrintableItemCode(code)) {
            FilterGuardFailures.fetch_add(1,std::memory_order_relaxed);
            return result;
        }
        snapshot = std::atomic_load_explicit(&PublishedFilterRules,
            std::memory_order_acquire);
        if (snapshot) {
            const auto ruleItem=GroundRuleItem(code,unit,snapshot.get(),header[2],
                GroundPropertyLive::Purpose::VerifiedLabel);
            GroundRuleDecision resolvedRule{};
            const auto* rule=ResolveGroundRule(snapshot.get(),ruleItem,resolvedRule) ?
                &resolvedRule : nullptr;
            UpdateMinimapProjectionIconRule(header[2],code,rule);
            if (rule && rule->hasName) {
                configuredName=rule->name.data();
                configuredBytes=rule->bytes;
            }
        }
        if (configuredName) FilterMatches.fetch_add(1,std::memory_order_relaxed);
    }
    constexpr std::size_t capacity = InnerNameBufferBytes - InnerNamePrefixBytes;
    const auto textAddress = outputAddress + InnerNamePrefixBytes;
    std::array<char, CandidateTextMaximum> original{};
    copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(textAddress),
            original.data(), original.size(), &copied) || copied != original.size()) {
        GeometryReadFailures.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const auto* end = static_cast<const char*>(
        std::memchr(original.data(), '\0', original.size()));
    if (!end || end == original.data()) {
        GeometryNoTerminator.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    GeometryObserved.fetch_add(1, std::memory_order_relaxed);
    const char* replacement = configuredName;
    std::size_t replacementBytes = configuredBytes;
    // This native writer is the Alt-visible ground label, unlike the SoE V2
    // hidden-hover relay above. Both take the current quantity from the same
    // borrowed TYPE_ITEM and add the prefix before native text measurement.
    std::array<char,InnerNameBufferBytes> countedName{};
    {
        const auto currentName=std::string_view(original.data(),
            static_cast<std::size_t>(end-original.data()));
        const auto displayName=configuredName ?
            std::string_view(configuredName,configuredBytes-1U):currentName;
        const auto quantity=GroundStackQuantity(unit);
        std::size_t countedBytes{};
        if (GroundQuantity::Append(displayName,quantity,
                countedName.data(),capacity,countedBytes)) {
            replacement=countedName.data();
            replacementBytes=countedBytes+1U;
        } else if (!configuredName) {
            // No rule and stat <=1 (or already presented by vanilla).
            FilterNoRule.fetch_add(1,std::memory_order_relaxed);
            GeometryNoRule.fetch_add(1,std::memory_order_relaxed);
            return result;
        }
    }
    if (replacementBytes > capacity) {
        GeometryReadFailures.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    MEMORY_BASIC_INFORMATION memory{};
    auto* destination = reinterpret_cast<void*>(textAddress);
    if (!VirtualQuery(destination, &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        GeometryReadFailures.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const auto protection = memory.Protect & 0xFFU;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) {
        GeometryReadFailures.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    const auto regionStart = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    if (regionStart > textAddress || memory.RegionSize > UINTPTR_MAX - regionStart ||
        regionStart + memory.RegionSize < textAddress ||
        regionStart + memory.RegionSize - textAddress < replacementBytes) {
        GeometryReadFailures.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    // Only the native formatter's TRANSIENT label record. Geometry is left
    // to the still-running original formatter (text measure + rectangle).
    std::memcpy(destination, replacement, replacementBytes);
    GeometryWrites.fetch_add(1, std::memory_order_relaxed);
    FilterWrites.fetch_add(1,std::memory_order_relaxed);
    return result;
}

void ArmInnerNameWriter() noexcept {
    if (InnerNameHookInstalled.load(std::memory_order_acquire)) {
        Emit("LOOT_GEOMETRY_HOOK_READY already-installed=1");
        return;
    }
    if (!Context || !D2RL::GetBuildName(Context) ||
        std::string_view(D2RL::GetBuildName(Context)) != "93847" ||
        !FormatterHookInstalled.load(std::memory_order_acquire) ||
        !OriginalLabelFormatter || !OriginalGetItemCode ||
        !HookInstalled.load(std::memory_order_acquire)) {
        Emit("LOOT_GEOMETRY_REFUSED formatter-arm and item-code hook required on build 93847");
        return;
    }
    std::array<std::uint8_t,5> call{};
    if (!ReadSafe(InnerNameWriterCallerRva,call.data(),call.size()) ||
        call != ExpectedInnerNameCall ||
        !Context->CheckExpectedBytes(InnerNameWriterRva,
            ExpectedInnerNameWriter.data(),
            static_cast<std::uint32_t>(ExpectedInnerNameWriter.size()))) {
        Emit("LOOT_GEOMETRY_REFUSED inner-name-callsite-or-prolog-fingerprint-mismatch; no-fallback-write");
        return;
    }
    if (!Context->InstallInlineHook(InnerNameWriterRva,
            ExpectedInnerNameWriter.data(),
            static_cast<std::uint32_t>(ExpectedInnerNameWriter.size()),
            HookInnerNameWriter,&OriginalInnerNameWriter) || !OriginalInnerNameWriter) {
        Emit("LOOT_GEOMETRY_REFUSED loader-hook-registration-failed; no-fallback-write");
        return;
    }
    InnerNameHookInstalled.store(true,std::memory_order_release);
    Emit("LOOT_GEOMETRY_HOOK_READY version=" UNHOARDER_VERSION_STRING " hook=D2R+0xCBEB0 onlyCallerReturnRva=0x1FAA1D innerSize=0x80 textOffset=+0x04 geometryRecalculatedByNativeFormatter=1 mode=off");
}

// Record identity at the only point with a verified native item pointer.
// The painter may read a separate label record, so match unit ID, recent
// formatter observation, and exact label text; NEVER compare record pointers.
void RememberGroundIdentity(void* unit, void* record, bool sourceCall,
                           bool paired, std::uint8_t result) noexcept {
    if (ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules ||
        !sourceCall || !paired || !result || !unit || !record ||
        !OriginalGetItemCode || !HookInstalled.load(std::memory_order_acquire)) return;
    std::uint32_t header[3]{},recordId{};
    SIZE_T copied{};
    const auto address=reinterpret_cast<std::uintptr_t>(record);
    if (address>UINTPTR_MAX-0x28-80 ||
        !ReadProcessMemory(GetCurrentProcess(),unit,header,sizeof(header),&copied) ||
        copied!=sizeof(header) || header[0]!=4 || header[2]==0) {
        return;
    }
    copied=0;
    if (!ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address+0x10),&recordId,
        sizeof(recordId),&copied) || copied!=sizeof(recordId) ||
        recordId!=header[2]) {
        return;
    }
    const auto code=CanonicalItemCode(OriginalGetItemCode(unit));
    const auto table=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (!table) return;
    const auto scalars=GroundRuleItem(code,unit,table.get(),recordId,
        GroundPropertyLive::Purpose::VerifiedLabel);
    GroundRuleDecision resolvedRule{};
    const auto* selected=ResolveGroundRule(table.get(),scalars,resolvedRule) ?
        &resolvedRule : nullptr;
    // Ordinary items may have no matching filter rule. Keep this guard
    // unconditional before any selected-> field access.
    if (!selected || (!selected->hasBackground &&
        !selected->hasTextColor && selected->show)) {
        return;
    }
    std::array<char,80> name{};
    copied=0;
    if (!ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address+0x28),name.data(),
        name.size(),&copied) || copied!=name.size() ||
        !std::memchr(name.data(),'\0',name.size())) {
        return;
    }
    const auto visibleLength=strnlen_s(name.data(),name.size());
    const auto quantity=GroundStackQuantity(unit); // exact formatter unit
    if (selected->hasName &&
        !GroundQuantity::MatchesRuleName(
            std::string_view(name.data(),visibleLength),
            std::string_view(selected->name.data(),selected->bytes-1U),
            quantity)) {
        return;
    }
    if (!GroundIdentityMutex.try_lock()) {
        return;
    }
    const auto now=GetTickCount64();
    const auto base=static_cast<std::size_t>(recordId)%IdentitySlots;
    std::size_t slot=IdentitySlots;
    for (std::size_t offset=0;offset<IdentityLookupWindow;offset++) {
        const auto pos=(base+offset)%IdentitySlots;
        const auto& candidate=GroundIdentities[pos];
        if (candidate.unitId==recordId || candidate.unitId==0 ||
            now-candidate.seenMs>IdentityTtlMs) { slot=pos; break; }
    }
    if (slot!=IdentitySlots) {
        GroundIdentities[slot]={recordId,code,header[1],now,name,quantity,
            scalars.qualityKnown,scalars.quality,
            scalars.itemLevelKnown,scalars.itemLevel,
            scalars.socketsKnown,scalars.sockets,
            scalars.etherealKnown,scalars.ethereal,
            scalars.identifiedKnown,scalars.identified};
    }
    GroundIdentityMutex.unlock();
}

bool GetGroundIdentity(std::uint32_t id, std::string_view visibleName,
                       std::uint32_t& code,
                       std::uint32_t* quantityOut,
                       std::uint32_t* classIdOut,
                       RuleEngine::Item* scalarOut) noexcept {
    if (!id || !GroundIdentityMutex.try_lock()) {
        return false;
    }
    const auto now=GetTickCount64();
    const auto base=static_cast<std::size_t>(id)%IdentitySlots;
    bool found=false;
    for (std::size_t offset=0;offset<IdentityLookupWindow;offset++) {
        const auto& candidate=GroundIdentities[(base+offset)%IdentitySlots];
        if (candidate.unitId!=id || now-candidate.seenMs>IdentityTtlMs) continue;
        const auto length=strnlen_s(candidate.visibleName.data(),candidate.visibleName.size());
        if (length==visibleName.size() &&
            std::memcmp(candidate.visibleName.data(),visibleName.data(),length)==0) {
            code=candidate.code;
            if (quantityOut) *quantityOut=candidate.quantity;
            if (classIdOut) *classIdOut=candidate.classId;
            if (scalarOut) {
                scalarOut->qualityKnown=candidate.qualityKnown;
                scalarOut->quality=candidate.quality;
                scalarOut->itemLevelKnown=candidate.itemLevelKnown;
                scalarOut->itemLevel=candidate.itemLevel;
                scalarOut->socketsKnown=candidate.socketsKnown;
                scalarOut->sockets=candidate.sockets;
                scalarOut->etherealKnown=candidate.etherealKnown;
                scalarOut->ethereal=candidate.ethereal;
                scalarOut->identifiedKnown=candidate.identifiedKnown;
                scalarOut->identified=candidate.identified;
            }
            found=true;
        }
        break;
    }
    GroundIdentityMutex.unlock();
    return found;
}

// Per-rule ground-label background and text-color forwarding. The hook only
// operates on the qualified build-93847 ground-label callsite. It does not
// persist writes into native label, glyph, item, or unit memory.
thread_local const float* VerifiedRuleGlyphColor=nullptr;
std::atomic_bool CorrectedGlyphBArmed{};

void __cdecl UnHoarderSharedLabelPaintMiddleware(
    void*,std::uintptr_t caller,
    void* rect,void* textArg,void* colorArg,
    TooltipCompat::SharedLabelPaintNextFn next,void* nextContext) noexcept {
    const bool ground=caller==Base+0x1517AF6;
    const bool rulesActive=ActiveGeometryMode.load(std::memory_order_acquire)==GeometryMode::Rules;
    const bool tint=rulesActive && BackgroundTintArmed.load(std::memory_order_acquire);
    const bool hide=rulesActive && HideGroundArmed.load(std::memory_order_acquire);
    const bool textColor=rulesActive && CorrectedGlyphBArmed.load(std::memory_order_acquire);

    std::array<float,4> scopedRuleTextColor{};
    bool hasScopedRuleTextColor=false;
    void* forwardedColor=colorArg;
    std::array<float,4> forwardedRgba{};
    bool concealGroundVisuals=false;

    if (ground && (tint || hide || textColor)) {
        const auto recordAddress=reinterpret_cast<std::uintptr_t>(rect);
        const auto textAddress=reinterpret_cast<std::uintptr_t>(textArg);
        const auto colorAddress=reinterpret_cast<std::uintptr_t>(colorArg);
        const bool paired=rect && textArg && colorArg &&
            recordAddress<=UINTPTR_MAX-0x28-80 &&
            textAddress==recordAddress+0x24 &&
            colorAddress==recordAddress+GroundColorOffset;
        std::array<float,4> rgba{};
        std::array<char,80> textBytes{};
        std::uint32_t recordId{};
        SIZE_T copied{};
        const bool colorOk=paired && ReadProcessMemory(GetCurrentProcess(),colorArg,
            rgba.data(),sizeof(rgba),&copied) && copied==sizeof(rgba);
        copied=0;
        const bool idOk=paired && ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(recordAddress+0x10),&recordId,
            sizeof(recordId),&copied) && copied==sizeof(recordId);
        copied=0;
        const bool textOk=paired && ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(recordAddress+0x28),textBytes.data(),
            textBytes.size(),&copied) && copied==textBytes.size() &&
            std::memchr(textBytes.data(),'\0',textBytes.size());
        std::uint32_t verifiedCode{},verifiedQuantity{},verifiedClassId{};
        RuleEngine::Item verifiedProperties{};
        const auto nameLength=textOk?strnlen_s(textBytes.data(),textBytes.size()):0;
        const bool identified=idOk && textOk &&
            GetGroundIdentity(recordId,std::string_view(textBytes.data(),nameLength),
                verifiedCode,&verifiedQuantity,&verifiedClassId,&verifiedProperties);

        if (paired && colorOk && identified) {
            const auto snapshot=std::atomic_load_explicit(&PublishedFilterRules,
                std::memory_order_acquire);
            const auto ruleItem=CachedGroundRuleItem(verifiedCode,verifiedClassId,
                verifiedQuantity,snapshot.get(),&verifiedProperties);
            GroundRuleDecision resolvedRule{};
            const auto* rule=ResolveGroundRule(snapshot.get(),ruleItem,resolvedRule) ?
                &resolvedRule : nullptr;
            const bool nameMatches=rule && (!rule->hasName ||
                GroundQuantity::MatchesRuleName(
                    std::string_view(textBytes.data(),nameLength),
                    std::string_view(rule->name.data(),rule->bytes-1U),
                    verifiedQuantity));

            if (hide && rule && CorrectedGlyphBArmed.load(std::memory_order_relaxed))
                concealGroundVisuals=GroundVisibility::ConcealBulkVisuals(
                    true,true,paired,colorOk,identified,!rule->show,nameMatches);

            if (textColor && rule && rule->hasTextColor && nameMatches) {
                scopedRuleTextColor=rule->textColor;
                hasScopedRuleTextColor=true;
            }

            if (tint && !concealGroundVisuals && rule && rule->hasBackground &&
                nameMatches && std::memcmp(rgba.data(),OriginalGroundBackground.data(),
                    sizeof(rgba))==0) {
                forwardedRgba=rule->background;
                forwardedColor=forwardedRgba.data();
            }
        }
    }

    std::array<float,4> invisibleRgba{{0.f,0.f,0.f,0.f}};
    if (concealGroundVisuals) forwardedColor=invisibleRgba.data();

    const float* const previousGlyphColor=VerifiedRuleGlyphColor;
    VerifiedRuleGlyphColor=concealGroundVisuals ? invisibleRgba.data() :
        (ground && hasScopedRuleTextColor?scopedRuleTextColor.data():nullptr);

    // A qualified show:false bulk-ground label omits the remainder of the
    // middleware chain. Pickup suppression is enforced independently.
    if (!concealGroundVisuals && next)
        next(nextContext,rect,textArg,forwardedColor);

    VerifiedRuleGlyphColor=previousGlyphColor;
}

void __fastcall HookSharedLabelPaint(void* rect,void* textArg,void* colorArg) noexcept {
    RunOwnedSharedLabelPaintChain(
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()),rect,textArg,colorArg);
}


// Build-93847 glyph renderer ABI: D2R+0x90857B passes RCX=context,
// XMM1/XMM2 scalar coordinates and R9=&float[4] color. The renderer moves R9
// into RDI at +0x32 before reading the RGBA values. Only this qualified glyph
// renderer is hooked; unrelated UI submission paths are left untouched.
using CorrectedGlyphBFn=std::uint64_t (__fastcall*)(
    void*,float,float,const float*) noexcept;
CorrectedGlyphBFn OriginalCorrectedGlyphB{};
std::atomic_bool CorrectedGlyphBInstalled{};
constexpr std::uintptr_t CorrectedGlyphBRva=0x658510;
constexpr std::uintptr_t CorrectedGlyphBCallRva=0x90857B;

using TooltipCompatServiceLease =
    D2RL::PluginCommunication::ServiceLease<TooltipCompat::Service>;

bool TooltipCompatOwnerMatches(
    const std::array<char,64>& owner,const D2RL::PluginContext* consumer) noexcept {
    if (!consumer || !consumer->pluginId || !consumer->pluginId[0]) return false;
    return std::strncmp(owner.data(),consumer->pluginId,owner.size())==0;
}

void TooltipCompatCopyOwner(
    std::array<char,64>& out,const D2RL::PluginContext* consumer) noexcept {
    out.fill(0);
    if (!consumer || !consumer->pluginId) return;
    std::snprintf(out.data(),out.size(),"%s",consumer->pluginId);
}

TooltipCompat::Result __cdecl RegisterCompatSharedLabelPaint(
    const D2RL::PluginContext* consumer,
    const TooltipCompat::SharedLabelPaintRegistration* registration,
    TooltipCompat::RegistrationHandle* handle) noexcept {
    if (handle) *handle=TooltipCompat::InvalidRegistrationHandle;
    if (!consumer || !consumer->pluginId || !registration || !handle ||
        registration->structSize<TooltipCompat::SharedLabelPaintRegistrationSize ||
        registration->flags!=0 || !registration->callback)
        return TooltipCompat::Result::InvalidArgument;
    if (SharedLabelPaintCompatRoute.load(std::memory_order_acquire)
            !=TooltipCompatRoute::NativeOwner)
        return TooltipCompat::Result::NotHost;

    try {
        std::scoped_lock lock(TooltipCompatMutex);
        const auto current=std::atomic_load_explicit(
            &TooltipPaintSubscribers,std::memory_order_acquire);
        auto next=std::make_shared<std::vector<TooltipPaintSubscriber>>(
            current?*current:std::vector<TooltipPaintSubscriber>{});
        for (const auto& entry:*next)
            if (entry.callback==registration->callback &&
                entry.userData==registration->userData &&
                TooltipCompatOwnerMatches(entry.owner,consumer))
                return TooltipCompat::Result::AlreadyRegistered;
        if (next->size()>=TooltipCompat::MaxSubscribers)
            return TooltipCompat::Result::LimitExceeded;
        TooltipPaintSubscriber entry{};
        entry.handle=TooltipCompatNextHandle++;
        if (!entry.handle) entry.handle=TooltipCompatNextHandle++;
        entry.callback=registration->callback;
        entry.userData=registration->userData;
        TooltipCompatCopyOwner(entry.owner,consumer);
        next->push_back(entry);
        *handle=entry.handle;
        std::atomic_store_explicit(
            &TooltipPaintSubscribers,
            std::shared_ptr<const std::vector<TooltipPaintSubscriber>>(std::move(next)),
            std::memory_order_release);
        return TooltipCompat::Result::Success;
    } catch (...) {
        return TooltipCompat::Result::LimitExceeded;
    }
}

TooltipCompat::Result __cdecl UnregisterCompatSharedLabelPaint(
    const D2RL::PluginContext* consumer,
    TooltipCompat::RegistrationHandle handle) noexcept {
    if (!consumer || !consumer->pluginId ||
        handle==TooltipCompat::InvalidRegistrationHandle)
        return TooltipCompat::Result::InvalidArgument;
    try {
        std::scoped_lock lock(TooltipCompatMutex);
        const auto current=std::atomic_load_explicit(
            &TooltipPaintSubscribers,std::memory_order_acquire);
        if (!current) return TooltipCompat::Result::NotFound;
        auto found=std::find_if(current->begin(),current->end(),
            [handle](const auto& entry){return entry.handle==handle;});
        if (found==current->end()) return TooltipCompat::Result::NotFound;
        if (!TooltipCompatOwnerMatches(found->owner,consumer))
            return TooltipCompat::Result::OwnerMismatch;
        auto next=std::make_shared<std::vector<TooltipPaintSubscriber>>(*current);
        next->erase(next->begin()+std::distance(current->begin(),found));
        std::atomic_store_explicit(
            &TooltipPaintSubscribers,
            std::shared_ptr<const std::vector<TooltipPaintSubscriber>>(std::move(next)),
            std::memory_order_release);
        return TooltipCompat::Result::Success;
    } catch (...) {
        return TooltipCompat::Result::LimitExceeded;
    }
}

TooltipCompat::Result __cdecl RegisterCompatGlyphRenderer(
    const D2RL::PluginContext* consumer,
    const TooltipCompat::GlyphRendererRegistration* registration,
    TooltipCompat::RegistrationHandle* handle) noexcept {
    if (handle) *handle=TooltipCompat::InvalidRegistrationHandle;
    if (!consumer || !consumer->pluginId || !registration || !handle ||
        registration->structSize<TooltipCompat::GlyphRendererRegistrationSize ||
        registration->flags!=0 || !registration->callback)
        return TooltipCompat::Result::InvalidArgument;
    if (GlyphRendererCompatRoute.load(std::memory_order_acquire)
            !=TooltipCompatRoute::NativeOwner)
        return TooltipCompat::Result::NotHost;

    try {
        std::scoped_lock lock(TooltipCompatMutex);
        const auto current=std::atomic_load_explicit(
            &TooltipGlyphSubscribers,std::memory_order_acquire);
        auto next=std::make_shared<std::vector<TooltipGlyphSubscriber>>(
            current?*current:std::vector<TooltipGlyphSubscriber>{});
        for (const auto& entry:*next)
            if (entry.callback==registration->callback &&
                entry.userData==registration->userData &&
                TooltipCompatOwnerMatches(entry.owner,consumer))
                return TooltipCompat::Result::AlreadyRegistered;
        if (next->size()>=TooltipCompat::MaxSubscribers)
            return TooltipCompat::Result::LimitExceeded;
        TooltipGlyphSubscriber entry{};
        entry.handle=TooltipCompatNextHandle++;
        if (!entry.handle) entry.handle=TooltipCompatNextHandle++;
        entry.callback=registration->callback;
        entry.userData=registration->userData;
        TooltipCompatCopyOwner(entry.owner,consumer);
        next->push_back(entry);
        *handle=entry.handle;
        std::atomic_store_explicit(
            &TooltipGlyphSubscribers,
            std::shared_ptr<const std::vector<TooltipGlyphSubscriber>>(std::move(next)),
            std::memory_order_release);
        return TooltipCompat::Result::Success;
    } catch (...) {
        return TooltipCompat::Result::LimitExceeded;
    }
}

TooltipCompat::Result __cdecl UnregisterCompatGlyphRenderer(
    const D2RL::PluginContext* consumer,
    TooltipCompat::RegistrationHandle handle) noexcept {
    if (!consumer || !consumer->pluginId ||
        handle==TooltipCompat::InvalidRegistrationHandle)
        return TooltipCompat::Result::InvalidArgument;
    try {
        std::scoped_lock lock(TooltipCompatMutex);
        const auto current=std::atomic_load_explicit(
            &TooltipGlyphSubscribers,std::memory_order_acquire);
        if (!current) return TooltipCompat::Result::NotFound;
        auto found=std::find_if(current->begin(),current->end(),
            [handle](const auto& entry){return entry.handle==handle;});
        if (found==current->end()) return TooltipCompat::Result::NotFound;
        if (!TooltipCompatOwnerMatches(found->owner,consumer))
            return TooltipCompat::Result::OwnerMismatch;
        auto next=std::make_shared<std::vector<TooltipGlyphSubscriber>>(*current);
        next->erase(next->begin()+std::distance(current->begin(),found));
        std::atomic_store_explicit(
            &TooltipGlyphSubscribers,
            std::shared_ptr<const std::vector<TooltipGlyphSubscriber>>(std::move(next)),
            std::memory_order_release);
        return TooltipCompat::Result::Success;
    } catch (...) {
        return TooltipCompat::Result::LimitExceeded;
    }
}

const TooltipCompat::Service PublishedTooltipCompatService{
    .serviceSize=TooltipCompat::ServiceSize,
    .serviceVersion=TooltipCompat::ServiceVersion,
    .registerSharedLabelPaint=&RegisterCompatSharedLabelPaint,
    .unregisterSharedLabelPaint=&UnregisterCompatSharedLabelPaint,
    .registerGlyphRenderer=&RegisterCompatGlyphRenderer,
    .unregisterGlyphRenderer=&UnregisterCompatGlyphRenderer,
};

const D2RL::PluginCommunicationService* ResolveTooltipCommunication() noexcept {
    if (TooltipCompatCommunication) return TooltipCompatCommunication;
    const D2RL::PluginCommunicationService* service{};
    if (!Context ||
        Context->QueryService(&service)!=D2RL::ServiceQueryResult::Success ||
        !D2RL::HasPluginCommunicationServiceField(
            service,D2RL::PluginCommunicationServiceRequiredSize))
        return nullptr;
    TooltipCompatCommunication=service;
    return service;
}

void PublishTooltipCompatService() noexcept {
    const auto* communication=ResolveTooltipCommunication();
    if (!communication || !communication->publishService) {
        Emit("LOOT_TOOLTIP_COMPAT_REFUSED reason=plugin-communication-unavailable");
        return;
    }
    const D2RL::PluginCommunication::PublishServiceRequest request{
        .structSize=D2RL::PluginCommunication::PublishServiceRequestSize,
        .flags=0,
        .name=TooltipCompat::ServiceName,
        .serviceVersion=TooltipCompat::ServiceVersion,
        .tableSize=TooltipCompat::ServiceSize,
        .table=&PublishedTooltipCompatService,
    };
    const auto result=communication->publishService(Context,&request);
    if (result==D2RL::PluginCommunication::Result::Success)
        Emit("LOOT_TOOLTIP_COMPAT_READY version=" UNHOARDER_VERSION_STRING " service=unhoarder-tooltip-compat abi=1 hooks=0x1FA8E0,0x658510 discovery=DiagnosticsService");
    else
        Emit("LOOT_TOOLTIP_COMPAT_REFUSED reason=publish-service-failed");
}

bool DiagnoseTrackedTooltipOwner(
    std::uintptr_t rva,const void* expected,std::uint32_t expectedSize,
    std::array<char,64>& owner) noexcept {
    owner.fill(0);
    const D2RL::DiagnosticsService* diagnostics{};
    if (!Context ||
        Context->QueryService(&diagnostics)!=D2RL::ServiceQueryResult::Success ||
        !D2RL::HasDiagnosticsServiceField(
            diagnostics,D2RL::DiagnosticsServiceRequiredSize) ||
        !diagnostics->queryHookStatus)
        return false;
    const D2RL::Diagnostics::HookQuery query{
        .structSize=D2RL::Diagnostics::HookQuerySize,
        .flags=0,
        .rva=rva,
        .expected=expected,
        .expectedSize=expectedSize,
        .reserved=0,
    };
    D2RL::Diagnostics::HookStatus status{
        .structSize=D2RL::Diagnostics::HookStatusSize,
    };
    if (diagnostics->queryHookStatus(Context,&query,&status)
            !=D2RL::Diagnostics::Result::Success ||
        status.state!=D2RL::Diagnostics::ModificationState::Tracked ||
        status.kind!=D2RL::Diagnostics::ModificationKind::InlineHook ||
        status.ownerCount!=1 || !status.ownerPluginId[0])
        return false;
    const auto length=strnlen_s(status.ownerPluginId,sizeof(status.ownerPluginId));
    if (!length || length>=owner.size()) return false;
    std::memcpy(owner.data(),status.ownerPluginId,length);
    owner[length]='\0';
    if (Context->pluginId &&
        std::string_view(owner.data())==std::string_view(Context->pluginId))
        return false;
    return true;
}

bool TryAttachForeignSharedLabelPaint(
    const void* expected,std::uint32_t expectedSize) noexcept {
    std::array<char,64> owner{};
    if (!DiagnoseTrackedTooltipOwner(
            TooltipCompat::SharedLabelPaintRva,expected,expectedSize,owner))
        return false;
    const auto* communication=ResolveTooltipCommunication();
    if (!communication) return false;
    TooltipCompatServiceLease lease;
    const auto acquired=D2RL::PluginCommunication::Acquire(
        Context,communication,owner.data(),TooltipCompat::ServiceName,
        TooltipCompat::ServiceVersion,TooltipCompat::ServiceRequiredSize,&lease);
    if (acquired!=D2RL::PluginCommunication::Result::Success ||
        !lease || !TooltipCompat::HasService(lease.Get()))
        return false;
    const TooltipCompat::SharedLabelPaintRegistration registration{
        .structSize=TooltipCompat::SharedLabelPaintRegistrationSize,
        .flags=0,
        .callback=&UnHoarderSharedLabelPaintMiddleware,
        .userData=nullptr,
    };
    TooltipCompat::RegistrationHandle handle{};
    if (lease->registerSharedLabelPaint(Context,&registration,&handle)
            !=TooltipCompat::Result::Success ||
        handle==TooltipCompat::InvalidRegistrationHandle)
        return false;
    ForeignPaintCompatLease=std::move(lease);
    ForeignPaintCompatHandle=handle;
    SharedLabelPaintCompatRoute.store(
        TooltipCompatRoute::ForeignHost,std::memory_order_release);
    BackgroundPaintHookInstalled.store(true,std::memory_order_release);
    char line[240]{};
    std::snprintf(line,sizeof(line),
        "LOOT_TOOLTIP_COMPAT_CHAINED point=shared-label-paint rva=0x1FA8E0 host=%s mode=foreign-host",
        owner.data());
    Emit(line);
    return true;
}

bool TryAttachForeignGlyphRenderer(
    const void* expected,std::uint32_t expectedSize) noexcept {
    std::array<char,64> owner{};
    if (!DiagnoseTrackedTooltipOwner(
            TooltipCompat::GlyphRendererRva,expected,expectedSize,owner))
        return false;
    const auto* communication=ResolveTooltipCommunication();
    if (!communication) return false;
    TooltipCompatServiceLease lease;
    const auto acquired=D2RL::PluginCommunication::Acquire(
        Context,communication,owner.data(),TooltipCompat::ServiceName,
        TooltipCompat::ServiceVersion,TooltipCompat::ServiceRequiredSize,&lease);
    if (acquired!=D2RL::PluginCommunication::Result::Success ||
        !lease || !TooltipCompat::HasService(lease.Get()))
        return false;
    const TooltipCompat::GlyphRendererRegistration registration{
        .structSize=TooltipCompat::GlyphRendererRegistrationSize,
        .flags=0,
        .callback=&UnHoarderGlyphRendererMiddleware,
        .userData=nullptr,
    };
    TooltipCompat::RegistrationHandle handle{};
    if (lease->registerGlyphRenderer(Context,&registration,&handle)
            !=TooltipCompat::Result::Success ||
        handle==TooltipCompat::InvalidRegistrationHandle)
        return false;
    ForeignGlyphCompatLease=std::move(lease);
    ForeignGlyphCompatHandle=handle;
    GlyphRendererCompatRoute.store(
        TooltipCompatRoute::ForeignHost,std::memory_order_release);
    CorrectedGlyphBInstalled.store(true,std::memory_order_release);
    CorrectedGlyphBArmed.store(true,std::memory_order_release);
    char line[240]{};
    std::snprintf(line,sizeof(line),
        "LOOT_TOOLTIP_COMPAT_CHAINED point=glyph-renderer rva=0x658510 host=%s mode=foreign-host",
        owner.data());
    Emit(line);
    return true;
}

void DetachForeignTooltipCompatRoutes() noexcept {
    if (ForeignGlyphCompatHandle && ForeignGlyphCompatLease &&
        ForeignGlyphCompatLease->unregisterGlyphRenderer && Context)
        (void)ForeignGlyphCompatLease->unregisterGlyphRenderer(
            Context,ForeignGlyphCompatHandle);
    ForeignGlyphCompatHandle=TooltipCompat::InvalidRegistrationHandle;
    (void)ForeignGlyphCompatLease.Reset();

    if (ForeignPaintCompatHandle && ForeignPaintCompatLease &&
        ForeignPaintCompatLease->unregisterSharedLabelPaint && Context)
        (void)ForeignPaintCompatLease->unregisterSharedLabelPaint(
            Context,ForeignPaintCompatHandle);
    ForeignPaintCompatHandle=TooltipCompat::InvalidRegistrationHandle;
    (void)ForeignPaintCompatLease.Reset();
}

struct TooltipPaintNextFrame final {
    std::shared_ptr<const std::vector<TooltipPaintSubscriber>> subscribers{};
    std::size_t nextIndex{};
    std::uintptr_t caller{};
    bool consumed{};
};

void InvokeCompatPaintSubscribers(
    const std::shared_ptr<const std::vector<TooltipPaintSubscriber>>& subscribers,
    std::size_t index,std::uintptr_t caller,
    void* rect,void* textArg,void* colorArg) noexcept;

void __cdecl ContinueCompatPaint(
    void* nextContext,void* rect,void* textArg,void* colorArg) noexcept {
    auto* frame=static_cast<TooltipPaintNextFrame*>(nextContext);
    if (!frame || frame->consumed) return;
    frame->consumed=true;
    InvokeCompatPaintSubscribers(
        frame->subscribers,frame->nextIndex,frame->caller,
        rect,textArg,colorArg);
}

void InvokeCompatPaintSubscribers(
    const std::shared_ptr<const std::vector<TooltipPaintSubscriber>>& subscribers,
    std::size_t index,std::uintptr_t caller,
    void* rect,void* textArg,void* colorArg) noexcept {
    if (!subscribers || index>=subscribers->size()) {
        if (OriginalSharedLabelPaint)
            OriginalSharedLabelPaint(rect,textArg,colorArg);
        return;
    }
    const auto& subscriber=(*subscribers)[index];
    if (!subscriber.callback) {
        InvokeCompatPaintSubscribers(
            subscribers,index+1,caller,rect,textArg,colorArg);
        return;
    }
    TooltipPaintNextFrame next{
        .subscribers=subscribers,
        .nextIndex=index+1,
        .caller=caller,
        .consumed=false,
    };
    subscriber.callback(
        subscriber.userData,caller,rect,textArg,colorArg,
        &ContinueCompatPaint,&next);
}

void RunOwnedSharedLabelPaintChain(
    std::uintptr_t caller,void* rect,void* textArg,void* colorArg) noexcept {
    const auto subscribers=std::atomic_load_explicit(
        &TooltipPaintSubscribers,std::memory_order_acquire);
    TooltipPaintNextFrame next{
        .subscribers=subscribers,
        .nextIndex=0,
        .caller=caller,
        .consumed=false,
    };
    UnHoarderSharedLabelPaintMiddleware(
        nullptr,caller,rect,textArg,colorArg,&ContinueCompatPaint,&next);
}

struct TooltipGlyphNextFrame final {
    std::shared_ptr<const std::vector<TooltipGlyphSubscriber>> subscribers{};
    std::size_t nextIndex{};
    std::uintptr_t caller{};
    bool consumed{};
    std::uint64_t result{};
};

std::uint64_t InvokeCompatGlyphSubscribers(
    const std::shared_ptr<const std::vector<TooltipGlyphSubscriber>>& subscribers,
    std::size_t index,std::uintptr_t caller,
    void* glyphContext,float x,float y,const float* rgba) noexcept;

std::uint64_t __cdecl ContinueCompatGlyph(
    void* nextContext,void* glyphContext,float x,float y,const float* rgba) noexcept {
    auto* frame=static_cast<TooltipGlyphNextFrame*>(nextContext);
    if (!frame) return 0;
    if (frame->consumed) return frame->result;
    frame->consumed=true;
    frame->result=InvokeCompatGlyphSubscribers(
        frame->subscribers,frame->nextIndex,frame->caller,
        glyphContext,x,y,rgba);
    return frame->result;
}

std::uint64_t InvokeCompatGlyphSubscribers(
    const std::shared_ptr<const std::vector<TooltipGlyphSubscriber>>& subscribers,
    std::size_t index,std::uintptr_t caller,
    void* glyphContext,float x,float y,const float* rgba) noexcept {
    if (!subscribers || index>=subscribers->size())
        return OriginalCorrectedGlyphB?
            OriginalCorrectedGlyphB(glyphContext,x,y,rgba):0;
    const auto& subscriber=(*subscribers)[index];
    if (!subscriber.callback)
        return InvokeCompatGlyphSubscribers(
            subscribers,index+1,caller,glyphContext,x,y,rgba);
    TooltipGlyphNextFrame next{
        .subscribers=subscribers,
        .nextIndex=index+1,
        .caller=caller,
        .consumed=false,
        .result=0,
    };
    return subscriber.callback(
        subscriber.userData,caller,glyphContext,x,y,rgba,
        &ContinueCompatGlyph,&next);
}

std::uint64_t RunOwnedGlyphRendererChain(
    std::uintptr_t caller,void* glyphContext,
    float x,float y,const float* rgba) noexcept {
    const auto subscribers=std::atomic_load_explicit(
        &TooltipGlyphSubscribers,std::memory_order_acquire);
    TooltipGlyphNextFrame next{
        .subscribers=subscribers,
        .nextIndex=0,
        .caller=caller,
        .consumed=false,
        .result=0,
    };
    return UnHoarderGlyphRendererMiddleware(
        nullptr,caller,glyphContext,x,y,rgba,&ContinueCompatGlyph,&next);
}

std::uint64_t __cdecl UnHoarderGlyphRendererMiddleware(
    void*,std::uintptr_t caller,
    void* context,float x,float y,const float* nativeRgba,
    TooltipCompat::GlyphRendererNextFn next,void* nextContext) noexcept {
    if (!next) return 0;
    if (caller!=Base+CorrectedGlyphBCallRva+5 ||
        !CorrectedGlyphBArmed.load(std::memory_order_relaxed) ||
        !VerifiedRuleGlyphColor || !nativeRgba)
        return next(nextContext,context,x,y,nativeRgba);
    std::array<float,4> before{};
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(),nativeRgba,before.data(),
            sizeof(before),&copied) || copied!=sizeof(before))
        return next(nextContext,context,x,y,nativeRgba);
    for (const auto value:before)
        if (!std::isfinite(value) || value<0.f || value>1.001f)
            return next(nextContext,context,x,y,nativeRgba);
    return next(nextContext,context,x,y,VerifiedRuleGlyphColor);
}

std::uint64_t __fastcall HookCorrectedGlyphB(
    void* context,float x,float y,const float* nativeRgba) noexcept {
    return RunOwnedGlyphRendererChain(
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()),
        context,x,y,nativeRgba);
}

void ArmCorrectedGlyphB() noexcept {
    if (CorrectedGlyphBInstalled.load(std::memory_order_acquire)) {
        CorrectedGlyphBArmed.store(true,std::memory_order_release);
        Emit("LOOT_GLYPH_B_ARMED version=" UNHOARDER_VERSION_STRING " already-installed=1 abi=RCX,XMM1,XMM2,R9-pointer hookA=0");
        return;
    }
    const char* build=Context ? D2RL::GetBuildName(Context) : nullptr;
    if (!Context || !build || std::string_view(build)!="93847" ||
        !FormatterHookInstalled.load(std::memory_order_acquire) ||
        !InnerNameHookInstalled.load(std::memory_order_acquire) ||
        !BackgroundPaintHookInstalled.load(std::memory_order_acquire) ||
        ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules) {
        Emit("LOOT_GLYPH_B_REFUSED version=" UNHOARDER_VERSION_STRING " reason=build-or-required-filter-hook-missing hooks=0");
        return;
    }
    constexpr std::array<std::uint8_t,5> nativeCall{0xE8,0x90,0xFF,0xD4,0xFF};
    // Qualified first 16 entry bytes for build 93847.
    constexpr std::array<std::uint8_t,16> nativeEntry{
        0x48,0x8B,0xC4,0x48,0x89,0x58,0x18,0x48,
        0x89,0x78,0x20,0x55,0x48,0x8D,0x68,0xA8};
    // Also verify B's actual pointer source (R9 -> RDI) at B+0x32.
    constexpr std::array<std::uint8_t,3> r9Consumer{0x49,0x8B,0xF9};
    constexpr std::array<std::uint8_t,7> colorPointerPrep{0x4C,0x8D,0x4D,0x18,0x48,0x8B,0xCB};
    std::array<std::uint8_t,5> call{};
    std::array<std::uint8_t,3> ptr{};
    std::array<std::uint8_t,7> prep{};
    if (!ReadSafe(CorrectedGlyphBCallRva,call.data(),call.size()) ||
        call!=nativeCall ||
        !ReadSafe(CorrectedGlyphBRva+0x32,ptr.data(),ptr.size()) ||
        ptr!=r9Consumer ||
        !ReadSafe(0x908525,prep.data(),prep.size()) || prep!=colorPointerPrep) {
        Emit("LOOT_GLYPH_B_REFUSED version=" UNHOARDER_VERSION_STRING " reason=callsite-or-R9-fingerprint-mismatch hooks=0");
        return;
    }
    if (!Context->CheckExpectedBytes(CorrectedGlyphBRva,nativeEntry.data(),
            static_cast<std::uint32_t>(nativeEntry.size()))) {
        if (TryAttachForeignGlyphRenderer(
                nativeEntry.data(),static_cast<std::uint32_t>(nativeEntry.size())))
            return;
        Emit("LOOT_GLYPH_B_REFUSED version=" UNHOARDER_VERSION_STRING " reason=entry-owned-without-unhoarder-tooltip-compat hooks=0");
        return;
    }
    if (!Context->InstallInlineHook(CorrectedGlyphBRva,nativeEntry.data(),
        static_cast<std::uint32_t>(nativeEntry.size()),HookCorrectedGlyphB,
        &OriginalCorrectedGlyphB) || !OriginalCorrectedGlyphB) {
        Emit("LOOT_GLYPH_B_REFUSED version=" UNHOARDER_VERSION_STRING " reason=loader-hook-install-failed hooks=0");
        return;
    }
    GlyphRendererCompatRoute.store(
        TooltipCompatRoute::NativeOwner,std::memory_order_release);
    CorrectedGlyphBInstalled.store(true,std::memory_order_release);
    CorrectedGlyphBArmed.store(true,std::memory_order_release);
    Emit("LOOT_GLYPH_B_ARMED version=" UNHOARDER_VERSION_STRING " target=D2R+0x658510 caller=D2R+0x90857B ABI=RCX-glyph,XMM1-float,XMM2-float,R9-float4-pointer onlyB=1 hookA=0 compatHost=1 recordWrites=0 itemWrites=0");
}

void EnsureSharedLabelPaintHook() noexcept;

void SyncFilterTextColorState() noexcept {
    const auto snapshot=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules ||
        !snapshot || !snapshot->textColorRules) {
        CorrectedGlyphBArmed.store(false,std::memory_order_release);
        Emit("LOOT_TEXT_COLOR_RULES_DISABLED reason=filter-off-or-no-textColor-rules");
        return;
    }
    // Even a text-only rule needs the verified shared painter to identify
    // the unit and put the correct per-item RGBA into TLS for native glyph B.
    if (!BackgroundPaintHookInstalled.load(std::memory_order_acquire))
        EnsureSharedLabelPaintHook();
    if (!BackgroundPaintHookInstalled.load(std::memory_order_acquire)) {
        CorrectedGlyphBArmed.store(false,std::memory_order_release);
        Emit("LOOT_TEXT_COLOR_RULES_REFUSED reason=shared-paint-hook-missing");
        return;
    }
    ArmCorrectedGlyphB();
    if (CorrectedGlyphBArmed.load(std::memory_order_acquire)) {
        char message[260]{};
        std::snprintf(message,sizeof(message),
            "LOOT_TEXT_COLOR_RULES_ARMED version=" UNHOARDER_VERSION_STRING " generation=%llu textColorRules=%zu source=runtime-json glyphB=only ABI=RCX,XMM1,XMM2,R9-pointer itemWrites=0",
            static_cast<unsigned long long>(snapshot->generation),snapshot->textColorRules);
        Emit(message);
    }
}

void EnsureSharedLabelPaintHook() noexcept {
    if (BackgroundPaintHookInstalled.load(std::memory_order_acquire)) return;
    const char* build=Context ? D2RL::GetBuildName(Context) : nullptr;
    constexpr std::array<std::uint8_t,17> expected{{
        0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,
        0x57,0x48,0x83,0xEC,0x40,0x49,0x8B}};
    constexpr std::array<std::uint8_t,5> groundCall{{0xE8,0xEA,0x2D,0xCE,0xFE}};
    constexpr std::array<std::uint8_t,5> neighborCall{{0xE8,0x9A,0x0A,0xCE,0xFE}};
    std::array<std::uint8_t,5> groundBytes{},neighborBytes{};
    if (!Context || !build || std::string_view(build)!="93847" ||
        !FormatterHookInstalled.load(std::memory_order_acquire) ||
        !OriginalLabelFormatter ||
        !ReadSafe(0x1517AF1,groundBytes.data(),groundBytes.size()) ||
        groundBytes!=groundCall ||
        !ReadSafe(0x1519E41,neighborBytes.data(),neighborBytes.size()) ||
        neighborBytes!=neighborCall) {
        Emit("LOOT_SHARED_LABEL_PAINT_HOOK_REFUSED build-or-callsite-fingerprint-mismatch; no-hook=1");
        return;
    }
    if (!Context->CheckExpectedBytes(0x1FA8E0,expected.data(),
            static_cast<std::uint32_t>(expected.size()))) {
        if (TryAttachForeignSharedLabelPaint(
                expected.data(),static_cast<std::uint32_t>(expected.size())))
            return;
        Emit("LOOT_SHARED_LABEL_PAINT_HOOK_REFUSED entry-owned-without-unhoarder-tooltip-compat; no-hook=1");
        return;
    }
    if (!Context->InstallInlineHook(0x1FA8E0,expected.data(),
        static_cast<std::uint32_t>(expected.size()),HookSharedLabelPaint,
        &OriginalSharedLabelPaint) || !OriginalSharedLabelPaint) {
        Emit("LOOT_SHARED_LABEL_PAINT_HOOK_REFUSED loader-install-failed; no-fallback=1");
        return;
    }
    SharedLabelPaintCompatRoute.store(
        TooltipCompatRoute::NativeOwner,std::memory_order_release);
    BackgroundPaintHookInstalled.store(true,std::memory_order_release);
    Emit("LOOT_SHARED_LABEL_PAINT_HOOK_READY version=" UNHOARDER_VERSION_STRING " hook=D2R+0x1FA8E0 returnSites=D2R+0x1517AF6,D2R+0x1519E46 nativeRGBA-qualified argument-override original-forwarded-once=1 compatHost=1");
}

// Fingerprint the proven label-color route before allowing any argument
// forwarding override. Recheck per-session once,
// not on every hot-path call. The label formatter itself is loader-hooked.
bool VerifyBackgroundColorRoute() noexcept {
    struct Fingerprint {
        std::uintptr_t rva;
        std::array<std::uint8_t,5> bytes;
        std::size_t length;
    };
    constexpr std::array<Fingerprint,7> checks{{
        {0x1517AE6,{0x4D,0x8D,0x47,0x14,0},4},
        {0x1519E36,{0x4D,0x8D,0x47,0x14,0},4},
        {0x1FA9A2,{0x48,0x89,0x5C,0x24,0x20},5},
        {0x1FA9AD,{0xE8,0xDE,0xD1,0x45,0x00},5},
        {0x657BBC,{0x48,0x8B,0x85,0x80,0x00},5},
        {0x657BD9,{0xF3,0x0F,0x10,0x48,0x0C},5},
        {0x657BEA,{0xF3,0x0F,0x10,0x00,0xB9},4},
    }};
    for (const auto& check:checks) {
        std::array<std::uint8_t,5> bytes{};
        if (!ReadSafe(check.rva,bytes.data(),check.length) ||
            std::memcmp(bytes.data(),check.bytes.data(),check.length)!=0)
            return false;
    }
    return true;
}

// Activate configurable backgrounds alongside the name filter. If the paint
// fingerprint/hook fails, names can remain armed, but colors stay disabled.
// Called at startup or on explicit reload, never in a native hook.
void SyncFilterBackgroundState() noexcept {
    const auto snapshot=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules ||
        !snapshot || !snapshot->backgroundRules) {
        BackgroundTintArmed.store(false,std::memory_order_release);
        Emit("LOOT_BACKGROUND_RULES_DISABLED reason=filter-off-or-no-background-rules");
        return;
    }
    if (!BackgroundPaintHookInstalled.load(std::memory_order_acquire))
        EnsureSharedLabelPaintHook();
    if (!BackgroundPaintHookInstalled.load(std::memory_order_acquire) ||
        !VerifyBackgroundColorRoute()) {
        BackgroundTintArmed.store(false,std::memory_order_release);
        Emit("LOOT_BACKGROUND_RULES_REFUSED native-paint-hook-or-fingerprint-mismatch names-remain-active=1");
        return;
    }
    BackgroundTintArmed.store(true,std::memory_order_release);
    char message[260]{};
    std::snprintf(message,sizeof(message),
        "LOOT_BACKGROUND_RULES_ARMED version=" UNHOARDER_VERSION_STRING " generation=%llu coloredRules=%zu source=runtime-json paint=argument-forward-only recordWrites=0 itemWrites=0",
        static_cast<unsigned long long>(snapshot->generation),snapshot->backgroundRules);
    Emit(message);
}


// Show:false needs the same exact ground-item painter/identity bridge as
// Alt-visible color. It does not require a backgroundColor rule. Refuse to
// hide anything if the native painter signature changes on a new build.
void SyncFilterVisibilityState() noexcept {
    HideGroundArmed.store(false,std::memory_order_release);
    const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules ||
        !rules || !rules->hiddenRules) return;
    if (!BackgroundPaintHookInstalled.load(std::memory_order_acquire))
        EnsureSharedLabelPaintHook();
    // Hiding requires both visual channels. A color-only mask would leave
    // unstyled native glyphs visible and must fail open instead.
    if (BackgroundPaintHookInstalled.load(std::memory_order_acquire) &&
        VerifyBackgroundColorRoute() &&
        !CorrectedGlyphBArmed.load(std::memory_order_acquire))
        ArmCorrectedGlyphB();
    if (!BackgroundPaintHookInstalled.load(std::memory_order_acquire) ||
        !VerifyBackgroundColorRoute() ||
        !CorrectedGlyphBInstalled.load(std::memory_order_acquire) ||
        !CorrectedGlyphBArmed.load(std::memory_order_acquire)) {
        Emit("LOOT_VISIBILITY_REFUSED native-painter-or-glyph-unavailable show:false-fails-open=1");
        return;
    }
    HideGroundArmed.store(true,std::memory_order_release);
    char message[250]{};
    std::snprintf(message,sizeof(message),
        "LOOT_VISIBILITY_READY version=" UNHOARDER_VERSION_STRING " hiddenRules=%zu source=JSON-show:false bulk-ground-native-painter-skip native-painter-forwarded=0-for-qualified-hidden-only key-state-independent=1 hover-native-path=qualified-hidden-row-skip pickup-state-writes=0",
        rules->hiddenRules);
    Emit(message);
}


// Per-game first-observed-unit sound alert. The Alt-ground-label and optional
// Qualified ground-label observations feed one registry. The public
// InventoryService is polled on the UI thread: seeing an existing, alerted
// unit ID in the player's owned inventory or cursor FORGETS precisely that ID.
// A later ground observation of the same unit can play another sound. This
// does not hook monster/TC drops or infer pickup from missing labels.
// Sound resolution still uses the qualified D2RLoader sounds.txt row-NAME
// backend on the game thread, not the sounds.txt numeric Index or FMOD.
const D2RL::ThreadService* SoundThreads{};
const D2RL::InventoryService* SoundInventory{};
const D2RL::ItemService* SoundInventoryItems{};
std::jthread SoundPickupPollWorker{};
std::atomic_bool SoundPickupPollPending{};
std::atomic<std::uint64_t> SoundPickupPolls{};
std::atomic<std::uint64_t> SoundPickupPollSuccess{};
std::atomic<std::uint64_t> SoundPickupPollRejected{};
std::atomic<std::uint64_t> SoundPickupResets{};
std::atomic<std::uint64_t> SoundPickupBusy{};
constexpr auto SoundPickupPollInterval=std::chrono::milliseconds(150);
std::atomic<std::uintptr_t> SoundLoaderBase{};
std::atomic_bool SoundArmed{};
std::atomic<ULONGLONG> SoundArmMs{};
std::atomic<std::uint64_t> SoundSeenTotal{};
std::atomic<std::uint64_t> SoundBaselineTotal{};
std::atomic<std::uint64_t> SoundQualifiedNew{};
std::atomic<std::uint64_t> SoundQueued{};
std::atomic<std::uint64_t> SoundQueueRejected{};
std::atomic<std::uint64_t> SoundPlayed{};
std::atomic<std::uint64_t> SoundUnknown{};
std::atomic<std::uint64_t> SoundCancelled{};
std::atomic<std::uint64_t> SoundCacheContention{};
std::atomic<std::uint64_t> SoundCacheFull{};
std::atomic<std::uint64_t> SoundAlreadySeen{};
std::atomic<std::uint64_t> SoundObservedAlt{};
std::atomic<std::uint64_t> SoundRegistryEpoch{1};
constexpr ULONGLONG SoundBaselineMs=1500;
SoundIdentity::Registry SoundSeenRegistry{};
std::mutex SoundSeenMutex;

bool SoundMemoryRead(std::uintptr_t addr,void* data,std::size_t size) noexcept {
    SIZE_T got{};
    return addr && size &&
        ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(addr),
            data,size,&got) && got==size;
}

[[nodiscard]] bool QualifyNamedSoundBackend(bool verbose) noexcept {
    // Both PE identities are known, but the 1.3.1 native layout is provisional:
    // enable ONLY if all old call-chain fingerprints and registration data
    // still match the LIVE executable. Never admit an image on its PE pair alone.
    SoundLoaderBase.store(0,std::memory_order_release);
    const auto mod=GetModuleHandleW(L"D2RLoader.exe");
    if(!mod) {
        if(verbose)Emit("LOOT_SOUND_QUALIFY refused=no-D2RLoader-image");
        return false;
    }
    const auto base=reinterpret_cast<std::uintptr_t>(mod);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    const bool peOk=SoundMemoryRead(base,&dos,sizeof(dos)) &&
        dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>=64 &&
        dos.e_lfanew<=4096 &&
        SoundMemoryRead(base+static_cast<std::uintptr_t>(dos.e_lfanew),&nt,sizeof(nt)) &&
        nt.Signature==IMAGE_NT_SIGNATURE &&
        nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    const auto layout=peOk?SoundLoaderIdentity::Classify(
        nt.FileHeader.TimeDateStamp,nt.OptionalHeader.SizeOfImage):
        SoundLoaderIdentity::Layout::Unknown;
    if(verbose) {
        char line[390]{};
        std::snprintf(line,sizeof(line),
            "LOOT_COMPAT_SOUND_LOADER version=" UNHOARDER_VERSION_STRING " peRead=%u "
            "stamp=0x%X imageSize=0x%X layout=%s "
            "oldPair=0x6AAFC972/0x5602000 "
            "loader131Pair=0x6AB3782C/0x5643000 "
            "nativeSoundCalls=%s",
            peOk?1U:0U,peOk?unsigned(nt.FileHeader.TimeDateStamp):0U,
            peOk?unsigned(nt.OptionalHeader.SizeOfImage):0U,
            SoundLoaderIdentity::Name(layout),
            layout==SoundLoaderIdentity::Layout::Unknown?"disabled":"pending-byte-qualification");
        Emit(line);
    }
    if(layout==SoundLoaderIdentity::Layout::Unknown) {
        if(verbose)Emit("LOOT_SOUND_QUALIFY refused=unrecognized-loader-image-pair audio-disabled=1");
        return false;
    }
    struct Witness {std::uint32_t offset;std::initializer_list<std::uint8_t> bytes;};
    const Witness witnesses[]{
        {0x1FCB26U,{0x48,0x8D,0x0D,0xA3,0x03,0x00,0x00}},
        {0x1FCED0U,{0x48,0x89,0x5C,0x24,0x18,0x57}},
        {0x1FCFD3U,{0x45,0x33,0xC0,0x33,0xD2}},
        {0x1FCFCDU,{0x41,0xB9,0x01,0x00,0x00,0x00}},
        {0x1FCFE3U,{0xE8,0x18,0x3C,0xFA,0xFF}},
        {0x1A0C00U,{0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57}},
        {0x1A0C18U,{0xE8,0x43,0x8B,0xF3,0xFF}},
        {0x1A0C43U,{0xE8,0x38,0xFB,0xFF,0xFF}},
    };
    std::size_t matched=0;
    for(const auto& witness:witnesses) {
        std::uint8_t found[16]{};
        const bool readOk=witness.bytes.size()<=sizeof(found) &&
            witness.offset<=nt.OptionalHeader.SizeOfImage &&
            witness.bytes.size()<=nt.OptionalHeader.SizeOfImage-witness.offset &&
            SoundMemoryRead(base+witness.offset,found,witness.bytes.size());
        const bool exact=readOk &&
            std::equal(witness.bytes.begin(),witness.bytes.end(),found);
        if(verbose) {
            char line[260]{};
            std::snprintf(line,sizeof(line),
                "LOOT_COMPAT_SOUND_WITNESS version=" UNHOARDER_VERSION_STRING " rva=0x%X "
                "readOk=%u exact=%u layout=%s",
                witness.offset,readOk?1U:0U,exact?1U:0U,
                SoundLoaderIdentity::Name(layout));
            Emit(line);
        }
        if(!exact) {
            if(verbose) {
                char line[180]{};
                std::snprintf(line,sizeof(line),
                    "LOOT_SOUND_QUALIFY refused=native-fingerprint-mismatch rva=0x%X",
                    witness.offset);
                Emit(line);
                char actual[3*16+1]{},expected[3*16+1]{};
                std::size_t i=0;
                for(const auto ch:witness.bytes) {
                    if(i>=16)break;
                    std::snprintf(actual+3*i,sizeof(actual)-3*i,"%02X ",unsigned(found[i]));
                    std::snprintf(expected+3*i,sizeof(expected)-3*i,"%02X ",unsigned(ch));
                    ++i;
                }
                char detail[330]{};
                std::snprintf(detail,sizeof(detail),
                    "LOOT_COMPAT_SOUND_BYTES rva=0x%X readOk=%u "
                    "expected=[%s] found=[%s] nativeSoundCalls=disabled",
                    witness.offset,readOk?1U:0U,expected,actual);
                Emit(detail);
            }
            return false;
        }
        ++matched;
    }
    // The three exact E8 witnesses above encode these three directed edges:
    // soundplay command callback -> named player -> name resolver / ID player.
    // Recheck the actual relative targets rather than treating byte similarity
    // alone as proof that the call reaches a qualified function.
    constexpr struct SoundCall {std::uint32_t at,target;} chain[]{
        {0x1FCFE3U,0x1A0C00U},
        {0x1A0C18U,0xD9760U},
        {0x1A0C43U,0x1A0780U},
    };
    for(const auto& edge:chain) {
        std::array<std::uint8_t,5> bytes{};
        std::int32_t displacement{};
        if(edge.at>nt.OptionalHeader.SizeOfImage-5 ||
           !SoundMemoryRead(base+edge.at,bytes.data(),bytes.size()) ||
           bytes[0]!=0xE8U) {
            if(verbose)Emit("LOOT_SOUND_QUALIFY refused=call-chain-unreadable audio-disabled=1");
            return false;
        }
        std::memcpy(&displacement,bytes.data()+1,sizeof(displacement));
        const auto actual=static_cast<std::int64_t>(edge.at)+5+displacement;
        if(actual!=static_cast<std::int64_t>(edge.target)) {
            if(verbose)Emit("LOOT_SOUND_QUALIFY refused=call-chain-target-mismatch audio-disabled=1");
            return false;
        }
    }
    char marker[10]{};
    if(!SoundMemoryRead(base+0x1CDFBB8U,marker,sizeof(marker)) ||
       std::memcmp(marker,"soundplay",sizeof(marker))!=0) {
        if(verbose)Emit("LOOT_SOUND_QUALIFY refused=soundplay-registration-name-mismatch");
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    const bool pageOk=VirtualQuery(reinterpret_cast<const void*>(base+0x1A0C00U),
            &mbi,sizeof(mbi))==sizeof(mbi) &&
        mbi.State==MEM_COMMIT &&
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))==0 &&
        ((mbi.Protect & 0xffU)==PAGE_EXECUTE ||
         (mbi.Protect & 0xffU)==PAGE_EXECUTE_READ ||
         (mbi.Protect & 0xffU)==PAGE_EXECUTE_READWRITE ||
         (mbi.Protect & 0xffU)==PAGE_EXECUTE_WRITECOPY);
    if(!pageOk) {
        if(verbose)Emit("LOOT_SOUND_QUALIFY refused=player-page-not-executable audio-disabled=1");
        return false;
    }
    // Only publish the native backend after the entire live contract matches.
    SoundLoaderBase.store(base,std::memory_order_release);
    if(verbose) {
        char msg[320]{};
        std::snprintf(msg,sizeof(msg),
            "LOOT_SOUND_QUALIFY matched=1 version=" UNHOARDER_VERSION_STRING " "
            "layout=%s nativeNamePlayer=D2RLoader+0x1A0C00 "
            "witnesses=%zu callChain=3 registration=exact executePage=1 "
            "nameBased=1 gameThreadRequired=1 rowIndexNotUsed=1",
            SoundLoaderIdentity::Name(layout),matched);
        Emit(msg);
    }
    return true;
}

struct SoundRequest {
    std::array<char,64> name{};
    std::uint32_t unitId{},code{};
    bool automatic{};
    std::uint64_t registryEpoch{}, itemTicket{};
};
void __cdecl PlayNamedSoundOnGameThread(
    const D2RL::PluginContext* logger,void* requestArg) noexcept {
    std::unique_ptr<SoundRequest> request(static_cast<SoundRequest*>(requestArg));
    if(!request || !request->name[0])return;
    if(request->automatic &&
       (!SoundArmed.load(std::memory_order_acquire) ||
        request->registryEpoch != SoundRegistryEpoch.load(std::memory_order_acquire))) {
        SoundCancelled.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    if (request->automatic) {
        // A pickup must cancel a queued old alert even if this unit is
        // re-dropped (and re-registered) within the same game epoch.
        if (!SoundSeenMutex.try_lock()) {
            SoundCancelled.fetch_add(1,std::memory_order_relaxed);
            return;
        }
        const bool valid = request->registryEpoch ==
            SoundRegistryEpoch.load(std::memory_order_acquire) &&
            SoundSeenRegistry.IsCurrent(request->unitId, request->itemTicket);
        SoundSeenMutex.unlock();
        if (!valid) {
            SoundCancelled.fetch_add(1,std::memory_order_relaxed);
            return;
        }
    }
    const auto base=SoundLoaderBase.load(std::memory_order_acquire);
    if(!base)return;
    using PlayNamedFn=void* (__fastcall*)(const char*,void*,int,int);
    auto* player=reinterpret_cast<PlayNamedFn>(base+0x1A0C00U);
    void* result=player(request->name.data(),nullptr,0,1);
    if(result)SoundPlayed.fetch_add(1,std::memory_order_relaxed);
    else SoundUnknown.fetch_add(1,std::memory_order_relaxed);
    char codeText[5]{};
    CodeText(request->code,codeText);
    char message[320]{};
    std::snprintf(message,sizeof(message),
        "LOOT_SOUND_PLAY version=" UNHOARDER_VERSION_STRING " trigger=%s code='%s' unitId=%u name='%s' engineReturned=%u gameThread=%lu note=return-not-audibility-proof",
        request->automatic?"first-ground-label":"manual-test",codeText,request->unitId,
        request->name.data(),result?1U:0U,static_cast<unsigned long>(GetCurrentThreadId()));
    if(logger) {
        logger->LogInfo(message);
    }
}

bool QueueNamedSound(std::string_view name,std::uint32_t unitId,
                     std::uint32_t code,bool automatic,
                     std::uint64_t observedEpoch=0,
                     std::uint64_t itemTicket=0) noexcept {
    if(name.empty() || name.size()>63 || !SoundThreads ||
       !SoundThreads->runOnGameThread || !Context ||
       !SoundLoaderBase.load(std::memory_order_acquire)) {
        SoundQueueRejected.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    auto* request=new(std::nothrow) SoundRequest{};
    if(!request) {
        SoundQueueRejected.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    std::memcpy(request->name.data(),name.data(),name.size());
    request->name[name.size()]='\0';
    request->unitId=unitId;request->code=code;request->automatic=automatic;
    request->registryEpoch=automatic?observedEpoch:
        SoundRegistryEpoch.load(std::memory_order_acquire);
    request->itemTicket=itemTicket;
    // The callback owns the raw allocation on Success. Never access it
    // after dispatch: the service may run the callback synchronously if we
    // are already on the game thread, and callback may have freed it.
    const auto codeResult=SoundThreads->runOnGameThread(
        Context,&PlayNamedSoundOnGameThread,request);
    if(codeResult!=D2RL::Threads::Result::Success) {
        delete request;
        SoundQueueRejected.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    SoundQueued.fetch_add(1,std::memory_order_relaxed);
    return true;
}

// Shared, read-only identity sink. The rule is resolved BEFORE claiming an ID;
// this keeps non-sound items out of the fixed-capacity sound registry. There
// is no visibility timeout, cooldown, or label-based replay heuristic.
void ObserveGroundSoundIdentity(std::uint32_t unitId,
    std::uint32_t rawCode,const void* nativeUnit,
    std::uint32_t knownClassId) noexcept {
    if (!SoundArmed.load(std::memory_order_acquire) || !unitId ||
        ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules)
        return;
    const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (!rules || !rules->soundRules) return;
    const auto code=CanonicalItemCode(rawCode);
    auto item=GroundRuleItem(code,nativeUnit,rules.get(),unitId,
        GroundPropertyLive::Purpose::VerifiedLabel);
    if(!item.classIdKnown && knownClassId!=0xffffffffU) {
        item.classIdKnown=true;item.classId=knownClassId;
    }
    GroundRuleDecision resolvedRule{};
    const auto* rule=ResolveGroundRule(rules.get(),item,resolvedRule) ?
        &resolvedRule : nullptr;
    if (!rule || !rule->hasDropSound) return;
    SoundObservedAlt.fetch_add(1,std::memory_order_relaxed);
    if (!SoundSeenMutex.try_lock()) {
        SoundCacheContention.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    const auto now=GetTickCount64();
    const auto start=SoundArmMs.load(std::memory_order_relaxed);
    const bool baseline=now>=start && now-start<SoundBaselineMs;
    const auto observedEpoch=SoundRegistryEpoch.load(std::memory_order_acquire);
    std::uint64_t itemTicket{};
    const auto observation=SoundSeenRegistry.Observe(unitId,code,&itemTicket);
    if (observation==SoundIdentity::Registry::Observation::New) {
        SoundSeenTotal.fetch_add(1,std::memory_order_relaxed);
        if (baseline) SoundBaselineTotal.fetch_add(1,std::memory_order_relaxed);
    } else if (observation==SoundIdentity::Registry::Observation::AlreadySeen) {
        SoundAlreadySeen.fetch_add(1,std::memory_order_relaxed);
    } else if (observation==SoundIdentity::Registry::Observation::Full) {
        SoundCacheFull.fetch_add(1,std::memory_order_relaxed);
    }
    SoundSeenMutex.unlock();
    if (observation!=SoundIdentity::Registry::Observation::New || baseline) return;
    SoundQualifiedNew.fetch_add(1,std::memory_order_relaxed);
    (void)QueueNamedSound(rule->dropSound.data(),unitId,code,true,
        observedEpoch,itemTicket);
}

void ExamineGroundSoundCandidate(void* unit,void* record,bool sourceCall,
                                  bool paired,std::uint8_t result) noexcept {
    if(!SoundArmed.load(std::memory_order_acquire) || !result || !sourceCall ||
       !paired || !unit || !record || !OriginalGetItemCode ||
       ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules)
        return;
    std::uint32_t header[4]{},recordId{};
    if(!SoundMemoryRead(reinterpret_cast<std::uintptr_t>(unit),header,sizeof(header)) ||
       header[0]!=4 || !header[2] ||
       !SoundMemoryRead(reinterpret_cast<std::uintptr_t>(record)+0x10,
                        &recordId,sizeof(recordId)) ||
       recordId!=header[2]) return;
    ObserveGroundSoundIdentity(header[2],OriginalGetItemCode(unit),unit,header[1]);
}

// Re-arm sounds ONLY after an explicit successful inventory/cursor
// observation, not on disappearance from the screen. The SDK returns copied
// ItemInfo snapshots, so no item handles/pointers escape these callbacks.
struct PickupPollResults final {
    std::uint32_t cleared{};
    std::uint32_t lastUnitId{};
};

void ForgetCarriedSoundItem(std::uint32_t unitId,
                            PickupPollResults& results) noexcept {
    if (!unitId || !SoundArmed.load(std::memory_order_acquire)) return;
    if (!SoundSeenMutex.try_lock()) {
        SoundPickupBusy.fetch_add(1,std::memory_order_relaxed);
        return; // A later poll will retry; never forcibly clear the registry.
    }
    const bool cleared=SoundSeenRegistry.Forget(unitId);
    SoundSeenMutex.unlock();
    if (cleared) {
        ++results.cleared;
        results.lastUnitId=unitId;
        SoundPickupResets.fetch_add(1,std::memory_order_relaxed);
    }
}

D2RL::Inventory::IterationAction __cdecl OnSoundInventoryItem(
    const D2RL::PluginContext*, const D2RL::Items::ItemInfo* item,
    void* userData) noexcept {
    if (!userData || !item ||
        item->structSize < D2RL::Items::ItemInfoRequiredSize ||
        item->container == D2RL::Items::ItemContainer::Ground)
        return D2RL::Inventory::IterationAction::Continue;
    ForgetMinimapProjectionItem(item->runtimeId);
    ForgetCarriedSoundItem(item->runtimeId,
        *static_cast<PickupPollResults*>(userData));
    return D2RL::Inventory::IterationAction::Continue;
}

void __cdecl PollSoundInventoryOnUiThread(
    const D2RL::PluginContext* context, void*) noexcept {
    // Release pending even when a game isn't ready or the sound rules are off.
    // Keep the flag true throughout the snapshot to avoid overlapping polls.
    const auto finish=[]() noexcept {
        SoundPickupPollPending.store(false,std::memory_order_release);
    };
    if (!context ||
        (!SoundArmed.load(std::memory_order_acquire) &&
         !AutomapProjectionHookInstalled.load(std::memory_order_acquire)) ||
        !SoundInventory || !SoundInventory->getLocalPlayer ||
        !SoundInventory->forEachInventoryItem) {
        finish();
        return;
    }
    SoundPickupPolls.fetch_add(1,std::memory_order_relaxed);
    D2RL::PlayerHandle player{};
    if (SoundInventory->getLocalPlayer(context,&player) !=
        D2RL::Inventory::Result::Success) {
        finish();
        return;
    }
    PickupPollResults results{};
    constexpr auto carriedMask =
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory) |
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Equipment) |
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Belt) |
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Cube) |
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::PersonalStash) |
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::SharedStash) |
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::CustomPage);
    const D2RL::Inventory::ItemFilter filter{
        D2RL::Inventory::ItemFilterSize,0,carriedMask,0
    };
    const auto enumerate = SoundInventory->forEachInventoryItem(
        context,player,&filter,&OnSoundInventoryItem,&results);
    // Cursor is not guaranteed to be included in inventory enumeration.
    // A ground pickup held on the cursor must also re-arm the original item.
    if (SoundInventory->getCursorItem && SoundInventoryItems &&
        SoundInventoryItems->getItemInfo) {
        D2RL::ItemHandle cursor{};
        if (SoundInventory->getCursorItem(context,player,&cursor) ==
            D2RL::Inventory::Result::Success) {
            D2RL::Items::ItemInfo info{};
            info.structSize=D2RL::Items::ItemInfoSize;
            if (SoundInventoryItems->getItemInfo(context,cursor,&info) ==
                    D2RL::Items::Result::Success &&
                info.structSize >= D2RL::Items::ItemInfoRequiredSize &&
                info.container == D2RL::Items::ItemContainer::Cursor)
                { ForgetMinimapProjectionItem(info.runtimeId);
                  ForgetCarriedSoundItem(info.runtimeId,results); }
        }
    }
    if (enumerate == D2RL::Inventory::Result::Success)
        SoundPickupPollSuccess.fetch_add(1,std::memory_order_relaxed);
    if (results.cleared) {
        char line[210]{};
        std::snprintf(line,sizeof(line),
            "LOOT_SOUND_PICKUP_REARM version=" UNHOARDER_VERSION_STRING " cleared=%u lastUnitId=%u "
            "source=public-inventory-and-cursor-snapshot no-native-hooks=1",
            results.cleared,results.lastUnitId);
        Emit(line);
    }
    finish();
}

void PollSoundInventoryLoop(std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(SoundPickupPollInterval);
        if (stop.stop_requested()) break;
        if (!Context ||
            (!SoundArmed.load(std::memory_order_acquire) &&
             !AutomapProjectionHookInstalled.load(std::memory_order_acquire)) ||
            !SoundInventory || !SoundThreads || !SoundThreads->runOnUiThread)
            continue;
        if (SoundPickupPollPending.exchange(true,std::memory_order_acq_rel))
            continue;
        if (SoundThreads->runOnUiThread(
                Context,&PollSoundInventoryOnUiThread,nullptr) !=
            D2RL::Threads::Result::Success) {
            SoundPickupPollPending.store(false,std::memory_order_release);
            SoundPickupPollRejected.fetch_add(1,std::memory_order_relaxed);
        }
    }
}

void StartSoundInventoryObserver() noexcept {
    if (!Context || !SoundThreads || !SoundThreads->runOnUiThread ||
        SoundPickupPollWorker.joinable()) return;
    const D2RL::InventoryService* inventory{};
    if (Context->QueryService(&inventory) !=
            D2RL::ServiceQueryResult::Success ||
        !D2RL::HasInventoryServiceField(inventory,
            D2RL::InventoryServiceRequiredSize) ||
        !inventory->getLocalPlayer || !inventory->forEachInventoryItem) {
        Emit("LOOT_SOUND_PICKUP_OBSERVER_UNAVAILABLE reason=public-inventory-service-missing "
             "sounds-continue=1 same-id-redrop=not-rearmed");
        return;
    }
    SoundInventory=inventory;
    const D2RL::ItemService* items{};
    if (Context->QueryService(&items) ==
            D2RL::ServiceQueryResult::Success &&
        D2RL::HasItemServiceField(items,D2RL::ItemServiceRequiredSize) &&
        items->getItemInfo && inventory->getCursorItem)
        SoundInventoryItems=items;
    try {
        SoundPickupPollWorker=std::jthread(&PollSoundInventoryLoop);
    } catch (const std::exception&) {
        SoundInventory=nullptr;
        SoundInventoryItems=nullptr;
        Emit("LOOT_SOUND_PICKUP_OBSERVER_UNAVAILABLE reason=worker-create-failed "
             "sounds-continue=1 same-id-redrop=not-rearmed");
        return;
    }
    char line[270]{};
    std::snprintf(line,sizeof(line),
        "LOOT_SOUND_PICKUP_OBSERVER_READY version=" UNHOARDER_VERSION_STRING " source=SDK-InventoryService "
        "intervalMs=150 includesCursor=%u nativeHooksAdded=0 inventoryWrites=0 "
        "audio=first-observed-after-pickup minimapPickupLifecycle=shared-read-only",
        SoundInventoryItems ? 1U:0U);
    Emit(line);
}

void ArmNewGroundSound() noexcept {
    SoundArmed.store(false,std::memory_order_release);
    auto rules=std::atomic_load_explicit(&PublishedFilterRules,std::memory_order_acquire);
    {
        char guard[340]{};
        std::snprintf(guard,sizeof(guard),
            "LOOT_COMPAT_SOUND_GUARD version=" UNHOARDER_VERSION_STRING " threadService=%u "
            "runOnGameThread=%u formatter=%u rulesMode=%u soundRules=%zu "
            "nextStep=qualify-native-soundplay-only-when-prerequisites-pass",
            SoundThreads?1U:0U,
            SoundThreads && SoundThreads->runOnGameThread?1U:0U,
            FormatterHookInstalled.load(std::memory_order_acquire)?1U:0U,
            ActiveGeometryMode.load(std::memory_order_acquire)==GeometryMode::Rules?1U:0U,
            rules?rules->soundRules:0);
        Emit(guard);
    }
    if(!Context || !SoundThreads || !SoundThreads->runOnGameThread ||
       !FormatterHookInstalled.load(std::memory_order_acquire) ||
       ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules ||
       !rules || !rules->soundRules || !QualifyNamedSoundBackend(true)) {
        Emit("LOOT_SOUND_REFUSED reason=needs-filter-arm-sound-rules-ThreadService-and-qualified-native-soundplay");
        return;
    }
    {
        std::lock_guard lock(SoundSeenMutex);
        SoundRegistryEpoch.fetch_add(1,std::memory_order_acq_rel);
        SoundSeenRegistry.Clear();
        SoundArmMs.store(GetTickCount64(),std::memory_order_release);
    }
    SoundArmed.store(true,std::memory_order_release);
    char message[340]{};
    std::snprintf(message,sizeof(message),
        "LOOT_SOUND_ARMED version=" UNHOARDER_VERSION_STRING " mode=session-unit-id-first-observed sources=alt-ground-label+hidden-hover soundRules=%zu baselineMs=%llu no-visibility-replay=1 same-id-redrop=rearm-on-inventory-observation no-item-writes=1 note=not-native-drop-event",
        rules->soundRules,static_cast<unsigned long long>(SoundBaselineMs));
    Emit(message);
}

// The automatic activation path is defined before the native formatter
// installer. Keep this forward declaration so MSVC can resolve the call.
void ArmLabelFormatter() noexcept;

// Common path for automatic startup and optional manual reload. The JSON parser
// publishes one complete immutable snapshot before any native label mutation is
// armed. Never call hook installers on the native rendering path.
bool ActivateConfiguredFilter(bool automatic) noexcept {
    const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (!rules) {
        Emit("LOOT_FILTER_AUTO_INACTIVE reason=no-valid-json");
        return false;
    }
    if (rules->rules.empty()) {
        ActiveGeometryMode.store(GeometryMode::Off,std::memory_order_release);
        BackgroundTintArmed.store(false,std::memory_order_release);
        HideGroundArmed.store(false,std::memory_order_release);
        CorrectedGlyphBArmed.store(false,std::memory_order_release);
        SoundArmed.store(false,std::memory_order_release);
        SoundRegistryEpoch.fetch_add(1,std::memory_order_acq_rel);
        Emit("LOOT_FILTER_AUTO_INACTIVE reason=valid-json-empty-rules");
        return false;
    }
    if (!Context || !OriginalGetItemCode ||
        !HookInstalled.load(std::memory_order_acquire)) {
        Emit("LOOT_FILTER_AUTO_INACTIVE reason=item-code-reader-unavailable");
        return false;
    }
    if (!FormatterHookInstalled.load(std::memory_order_acquire))
        ArmLabelFormatter();
    if (!FormatterHookInstalled.load(std::memory_order_acquire)) {
        Emit("LOOT_FILTER_AUTO_INACTIVE reason=ground-label-formatter-unavailable");
        return false;
    }
    if (!InnerNameHookInstalled.load(std::memory_order_acquire))
        ArmInnerNameWriter();
    if (!InnerNameHookInstalled.load(std::memory_order_acquire) ||
        !OriginalInnerNameWriter) {
        Emit("LOOT_FILTER_AUTO_INACTIVE reason=ground-name-writer-unavailable");
        return false;
    }
    // Preserve the per-game observed-item sound registry across an in-session
    // rules reload: already heard items must not replay merely because JSON
    // was saved. Cancel queued pre-reload sounds via the epoch.
    const bool preserveSoundHistory=!automatic &&
        SoundArmed.load(std::memory_order_acquire) &&
        SoundLoaderBase.load(std::memory_order_acquire)!=0 &&
        rules->soundRules!=0;
    ActiveGeometryMode.store(GeometryMode::Rules,std::memory_order_release);
    SyncFilterBackgroundState();
    SyncFilterTextColorState();
    SyncFilterVisibilityState();
    // New files/initial activation use the existing first-observed baseline.
    // For live reloads, keep already-observed sound IDs while cancelling any
    // sounds queued from the previous generation (the old epoch).
    SoundArmed.store(false,std::memory_order_release);
    if (preserveSoundHistory) {
        SoundRegistryEpoch.fetch_add(1,std::memory_order_acq_rel);
        SoundArmed.store(true,std::memory_order_release);
        Emit("LOOT_RELOAD_SOUND retained-seen-ids=1 previous-queued-cancelled=1");
    } else if (rules->soundRules) ArmNewGroundSound();
    else if (!automatic) SoundRegistryEpoch.fetch_add(1,std::memory_order_acq_rel);
    const auto nameRules=static_cast<std::size_t>(std::count_if(
        rules->rules.begin(),rules->rules.end(),
        [](const FilterNameRule& rule) noexcept { return rule.hasName; }));
    char message[390]{};
    std::snprintf(message,sizeof(message),
        "LOOT_FILTER_AUTO_ACTIVE version=" UNHOARDER_VERSION_STRING " trigger=%s generation=%llu rules=%zu nameRules=%zu hiddenRules=%zu backgrounds=%u textColors=%u sound=%u runtime=standalone",
        automatic?"valid-json-startup":"valid-json-reload",
        static_cast<unsigned long long>(rules->generation),rules->rules.size(),nameRules,
        rules->hiddenRules,BackgroundTintArmed.load(std::memory_order_acquire)?1U:0U,
        CorrectedGlyphBArmed.load(std::memory_order_acquire)?1U:0U,
        SoundArmed.load(std::memory_order_acquire)?1U:0U);
    FilterLiveReloadAvailable.store(true,std::memory_order_release);
    Emit(message);
    return true;
}

// Only this worker-owned entrypoint requests a live reload. Successful JSON
// parsing publishes one immutable table; errors leave the previous table
// unchanged. The filter must have been activated at initial startup so we
// never install a first set of native rendering hooks on the polling worker.
bool TryLiveFilterReload(const char* trigger) noexcept {
    const auto previous=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (!Context || !previous ||
        !FilterLiveReloadAvailable.load(std::memory_order_acquire) ||
        !HookInstalled.load(std::memory_order_acquire) ||
        !FormatterHookInstalled.load(std::memory_order_acquire) ||
        !InnerNameHookInstalled.load(std::memory_order_acquire)) {
        FilterReloadRefused.fetch_add(1,std::memory_order_relaxed);
        char message[260]{};
        std::snprintf(message,sizeof(message),
            "LOOT_RELOAD_REFUSED trigger=%s reason=requires-previously-active-filter "
            "previous-rules-preserved=1 action=restart-with-valid-json",
            trigger);
        Emit(message);
        return false;
    }
    if (!ReloadFilterRules()) {
        FilterReloadRefused.fetch_add(1,std::memory_order_relaxed);
        char message[260]{};
        std::snprintf(message,sizeof(message),
            "LOOT_RELOAD_REFUSED trigger=%s reason=invalid-new-json-or-excel "
            "previousGeneration=%llu previous-rules-preserved=1",
            trigger,static_cast<unsigned long long>(previous->generation));
        Emit(message);
        return false;
    }
    const auto current=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    // Invalidate copied ground-label identity before re-arming the new rules.
    {
        std::lock_guard lock(GroundIdentityMutex);
        GroundIdentities.fill({}); // no old styled-name/paint identity survives
    }
    const bool activated=ActivateConfiguredFilter(false);
    if (!activated && (!current || !current->rules.empty())) {
        // A backend became unavailable between validation and activation.
        // Restore the last complete snapshot and its original arming state.
        std::atomic_store_explicit(&PublishedFilterRules,previous,
            std::memory_order_release);
        (void)ActivateConfiguredFilter(false);
        FilterReloadRefused.fetch_add(1,std::memory_order_relaxed);
        char message[270]{};
        std::snprintf(message,sizeof(message),
            "LOOT_RELOAD_REFUSED trigger=%s reason=reactivation-unavailable "
            "previousGeneration=%llu previous-rules-restored=1",
            trigger,static_cast<unsigned long long>(previous->generation));
        Emit(message);
        return false;
    }
    FilterReloadSucceeded.fetch_add(1,std::memory_order_relaxed);
    char message[340]{};
    std::snprintf(message,sizeof(message),
        "LOOT_RELOAD_OK version=" UNHOARDER_VERSION_STRING " trigger=%s previousGeneration=%llu "
        "generation=%llu schema=%u rules=%zu hiddenRules=%zu "
        "backgroundRules=%zu textColorRules=%zu soundRules=%zu "
        "pickupCallerPolicy=none",
        trigger,static_cast<unsigned long long>(previous->generation),
        static_cast<unsigned long long>(current?current->generation:0),
        current?current->schema:0U,current?current->rules.size():0U,
        current?current->hiddenRules:0U,current?current->backgroundRules:0U,
        current?current->textColorRules:0U,current?current->soundRules:0U);
    Emit(message);
    return true;
}

// Native IDs can be recycled between games. In automatic mode each game
// needs its own first-seen sound baseline (and no stale replay cache).
void RebaselineSoundAtGameJoin() noexcept {
    if (!SoundArmed.load(std::memory_order_acquire)) return;
    {
        std::lock_guard lock(SoundSeenMutex);
        SoundRegistryEpoch.fetch_add(1,std::memory_order_acq_rel);
        SoundSeenRegistry.Clear();
        SoundArmMs.store(GetTickCount64(),std::memory_order_release);
    }
    Emit("LOOT_SOUND_GAME_BASELINE_RESET auto=1 previously-observed-items-baselined-for-1500ms=1");
}

// The producer passes six ABI arguments, including two stack arguments.
// The formatter hook forwards all six ABI arguments exactly once and only
// feeds verified ground-item identity to production filter services.
std::uint8_t __fastcall HookLabelFormatter(
    void* unit, void* dest, void* record, std::uint32_t flags,
    std::uint64_t stack5, std::uint64_t stack6) noexcept {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool sourceCall =
        caller == Base + LabelFormatterCallRva + DirectCallBytes ||
        caller == Base + LabelFormatterSecondCallRva + DirectCallBytes;
    const auto destAddress = reinterpret_cast<std::uintptr_t>(dest);
    const auto recordAddress = reinterpret_cast<std::uintptr_t>(record);
    const bool paired = record && dest &&
        recordAddress <= static_cast<std::uintptr_t>(-1) - 0x24 &&
        destAddress == recordAddress + 0x24;

    const auto result = OriginalLabelFormatter(
        unit, dest, record, flags, stack5, stack6);
    FormatterCalls.fetch_add(1, std::memory_order_relaxed);
    RememberGroundIdentity(unit, record, sourceCall, paired, result);
    ExamineGroundSoundCandidate(unit, record, sourceCall, paired, result);
    return result;
}

void ArmLabelFormatter() noexcept {
    if (FormatterHookInstalled.load(std::memory_order_acquire)) {
        Emit("LOOT_FORMATTER_OBSERVER_READY already-installed=1");
        return;
    }
    const char* build = Context ? D2RL::GetBuildName(Context) : nullptr;
    if (!Context || !build || std::string_view(build) != "93847" ||
        !Base || !ImageSize) {
        Emit("LOOT_FORMATTER_REFUSED unexpected-build-or-uninitialized-image");
        return;
    }
    std::array<std::uint8_t,5> call{};
    std::array<std::uint8_t,5> secondCall{};
    if (!ReadSafe(LabelFormatterCallRva,call.data(),call.size()) ||
        call != ExpectedLabelFormatterCaller ||
        !ReadSafe(LabelFormatterSecondCallRva,secondCall.data(),secondCall.size()) ||
        secondCall != ExpectedLabelFormatterSecondCaller ||
        !Context->CheckExpectedBytes(LabelFormatterRva,
            ExpectedLabelFormatter.data(),
            static_cast<std::uint32_t>(ExpectedLabelFormatter.size()))) {
        Emit("LOOT_FORMATTER_REFUSED callsite-or-formatter-fingerprint-mismatch-or-owned-hook; no-fallback-write");
        return;
    }
    if (!Context->InstallInlineHook(LabelFormatterRva,
            ExpectedLabelFormatter.data(),
            static_cast<std::uint32_t>(ExpectedLabelFormatter.size()),
            HookLabelFormatter,&OriginalLabelFormatter) ||
        !OriginalLabelFormatter) {
        Emit("LOOT_FORMATTER_REFUSED loader-hook-registration-failed; no-fallback-write");
        return;
    }
    FormatterHookInstalled.store(true,std::memory_order_release);
    Emit("LOOT_FORMATTER_OBSERVER_READY version=" UNHOARDER_VERSION_STRING " hook=D2R+0x1FA9F0 sourceCalls=D2R+0x15171E5,D2R+0x1517783 expectedReturnRvas=0x15171EA,0x1517788 sixArgsForwarded=1 labelWrites=none nativeCodeWrites=loader-managed-hook-only");
}

// Build-93847 native pickup guard. The only intercepted action is native
// item pickup (action 22, unit type 4). A pickup is blocked only after a fresh
// native lookup proves the requested unit is still an on-ground item and the
// current filter resolves it to Hide. Every uncertain read fails open.
namespace PickupGuard = NativePickupGuardPolicy;
constexpr std::uintptr_t NativeActionDispatchRva=0xFABE0;
constexpr std::uintptr_t NativeItemLookupRva=0x9A5D0;
constexpr std::uintptr_t NativeVerifiedPickupCallRva=0x101ADF;
using NativeItemLookupFn=void*(__fastcall*)(std::uint32_t,std::uint32_t) noexcept;
using NativeActionDispatchFn=void(__fastcall*)(std::uint32_t,void*,
    std::uint32_t,std::uint32_t) noexcept;
NativeActionDispatchFn OriginalNativeActionDispatch{};
std::atomic_bool NativePickupGuardQualified{};
std::atomic_bool NativeActionDispatchInstalled{};
thread_local bool NativePickupGuardInside=false;
std::jthread RuntimeWorker{};

PickupGuard::Decision QualifyGroundPickup(std::uint32_t action,void* player,
    std::uint32_t type,std::uint32_t id) noexcept {
    if(!NativePickupGuardQualified.load(std::memory_order_acquire))
        return PickupGuard::Decision::GuardInactive;
    if(!PickupGuard::Candidate(action,type,id))
        return PickupGuard::Decision::InvalidTargetId;
    if(!HideGroundArmed.load(std::memory_order_acquire))
        return PickupGuard::Decision::VisibilityNotArmed;
    if(ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules)
        return PickupGuard::Decision::RulesModeInactive;
    if(!HookInstalled.load(std::memory_order_acquire) || !OriginalGetItemCode)
        return PickupGuard::Decision::CodeReaderUnavailable;
    if(!player) return PickupGuard::Decision::NullPlayer;
    if(NativePickupGuardInside) return PickupGuard::Decision::Reentrant;

    const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if(!rules || !rules->hiddenRules)
        return PickupGuard::Decision::NoHiddenRules;

    std::uint32_t playerType=0xffffffffU;
    SIZE_T copied{};
    if(!ReadProcessMemory(GetCurrentProcess(),player,&playerType,
            sizeof(playerType),&copied) || copied!=sizeof(playerType) ||
       playerType!=0)
        return PickupGuard::Decision::InvalidPlayer;

    NativePickupGuardInside=true;
    auto* unit=reinterpret_cast<NativeItemLookupFn>(Base+NativeItemLookupRva)(
        id,PickupGuard::ItemUnitType);
    NativePickupGuardInside=false;
    if(!unit) return PickupGuard::Decision::LookupFailed;

    std::array<std::uint32_t,4> header{};
    copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),unit,header.data(),
            sizeof(header),&copied) || copied!=sizeof(header) ||
       !PickupGuard::SameItemIdentity(header[0],header[2],id))
        return PickupGuard::Decision::InvalidUnit;
    if(!PickupGuard::GroundMode(header[3]))
        return PickupGuard::Decision::NotGround;

    NativePickupGuardInside=true;
    const auto code=CanonicalItemCode(OriginalGetItemCode(unit));
    NativePickupGuardInside=false;
    if(!PrintableItemCode(code))
        return PickupGuard::Decision::InvalidCode;

    const auto ruleItem=GroundRuleItem(code,unit,rules.get(),id);
    GroundRuleDecision resolvedRule{};
    const auto* matchedRule=ResolveGroundRule(rules.get(),ruleItem,resolvedRule) ?
        &resolvedRule : nullptr;
    if(!matchedRule || matchedRule->show)
        return PickupGuard::Decision::NoHiddenRule;
    return PickupGuard::Decision::Blocked;
}

void __fastcall HookNativeActionDispatch(std::uint32_t action,void* player,
    std::uint32_t type,std::uint32_t id) noexcept {
    if(action==PickupGuard::PickupAction && type==PickupGuard::ItemUnitType &&
       QualifyGroundPickup(action,player,type,id)==PickupGuard::Decision::Blocked)
        return;
    OriginalNativeActionDispatch(action,player,type,id);
}

bool NativeCallMatches(std::uintptr_t callRva,
    std::uintptr_t targetRva) noexcept {
    std::array<std::uint8_t,5> bytes{};
    if(!ReadSafe(callRva,bytes.data(),bytes.size()) || bytes[0]!=0xE8)
        return false;
    std::int32_t relative{};
    std::memcpy(&relative,bytes.data()+1,sizeof(relative));
    return static_cast<std::int64_t>(callRva)+5+relative==
        static_cast<std::int64_t>(targetRva);
}

void InstallNativePickupGuard() noexcept {
    if(!Context || !Base || !ImageSize ||
       !D2RL::GetBuildName(Context) ||
       std::string_view(D2RL::GetBuildName(Context))!="93847"){
        Emit("LOOT_PICKUP_GUARD_INACTIVE version=" UNHOARDER_VERSION_STRING " reason=wrong-build-or-image forward-only=1");
        return;
    }

    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if(!ReadSafe(0,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
       dos.e_lfanew<=0 || dos.e_lfanew>0x1000 ||
       !ReadSafe(static_cast<std::uintptr_t>(dos.e_lfanew),&nt,sizeof(nt)) ||
       nt.Signature!=IMAGE_NT_SIGNATURE ||
       nt.FileHeader.TimeDateStamp!=0x6AB3782C ||
       nt.OptionalHeader.SizeOfImage!=ImageSize){
        Emit("LOOT_PICKUP_GUARD_INACTIVE version=" UNHOARDER_VERSION_STRING " reason=PE-timestamp-or-image-size-mismatch forward-only=1");
        return;
    }

    // These build-93847 call edges qualify both the immediate and deferred
    // routes into the dispatch function, plus the unit-by-id lookup used by
    // that dispatch. They are validation witnesses only; caller provenance is
    // never used to authorize a block.
    constexpr std::array<std::uintptr_t,3> dispatchWitnesses{{
        0xFA115,0xFBF30,NativeVerifiedPickupCallRva
    }};
    for(const auto callRva:dispatchWitnesses){
        if(!NativeCallMatches(callRva,NativeActionDispatchRva)){
            Emit("LOOT_PICKUP_GUARD_INACTIVE version=" UNHOARDER_VERSION_STRING " reason=dispatch-callsite-witness-mismatch forward-only=1");
            return;
        }
    }
    constexpr std::array<std::uint8_t,3> lookupEntry{{0x4C,0x63,0xCA}};
    if(!NativeCallMatches(0xFACB9,NativeItemLookupRva) ||
       !Context->CheckExpectedBytes(NativeItemLookupRva,
            lookupEntry.data(),static_cast<std::uint32_t>(lookupEntry.size()))){
        Emit("LOOT_PICKUP_GUARD_INACTIVE version=" UNHOARDER_VERSION_STRING " reason=item-lookup-witness-mismatch forward-only=1");
        return;
    }

    // Take an exact live snapshot only after the immutable build/call-graph
    // witnesses above pass. Reject an entry already owned by an obvious bridge;
    // the loader performs the final expected-byte check during registration.
    std::array<std::uint8_t,20> dispatchEntry{};
    if(!ReadSafe(NativeActionDispatchRva,dispatchEntry.data(),
            dispatchEntry.size()) ||
       !((dispatchEntry[0]==0x40)||(dispatchEntry[0]==0x48)||
         (dispatchEntry[0]>=0x50 && dispatchEntry[0]<=0x57)) ||
       (dispatchEntry[0]==0x48 && dispatchEntry[1]==0xFF &&
        dispatchEntry[2]==0x25) ||
       !Context->CheckExpectedBytes(NativeActionDispatchRva,
            dispatchEntry.data(),
            static_cast<std::uint32_t>(dispatchEntry.size()))){
        Emit("LOOT_PICKUP_GUARD_INACTIVE version=" UNHOARDER_VERSION_STRING " reason=dispatch-entry-unqualified forward-only=1");
        return;
    }

    if(!Context->InstallInlineHook(NativeActionDispatchRva,
            dispatchEntry.data(),static_cast<std::uint32_t>(dispatchEntry.size()),
            HookNativeActionDispatch,&OriginalNativeActionDispatch) ||
       !OriginalNativeActionDispatch){
        Emit("LOOT_PICKUP_GUARD_INACTIVE version=" UNHOARDER_VERSION_STRING " reason=dispatch-hook-registration-failed forward-only=1");
        return;
    }

    NativeActionDispatchInstalled.store(true,std::memory_order_release);
    NativePickupGuardQualified.store(true,std::memory_order_release);
    Emit("LOOT_PICKUP_GUARD_READY version=" UNHOARDER_VERSION_STRING " action=22 type=4 mode=3 "
         "freshLookup=D2R+0x9A5D0 rule=show:false failOpen=1 stateWrites=0");
}

void RuntimeWorkerLoop(std::stop_token stop) noexcept {
    bool lastReloadChord=false;
    FilterLiveReload::Watcher ruleFileWatcher{};
    FilterLiveReload::Watcher weaponTableWatcher{},armorTableWatcher{};
    FilterLiveReload::Watcher miscTableWatcher{},itemTypesTableWatcher{};
    std::filesystem::path watchedBaseNameExcel{};
    bool watchedItemTypeCatalog{};
    ULONGLONG lastRuleFileCheck=0;

    while(!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if(stop.stop_requested()) break;
        const auto now=GetTickCount64();

        // Automatic JSON/table reload runs only on this worker; native item
        // and renderer callbacks never perform filesystem or parser work.
        if(!FilterConfigPath.empty() && now-lastRuleFileCheck>=200) {
            lastRuleFileCheck=now;
            if(ruleFileWatcher.Observe(ReadFilterFileStamp(FilterConfigPath),now,650)) {
                Emit("LOOT_RELOAD_REQUEST version=" UNHOARDER_VERSION_STRING " trigger=stable-file-change settleMs=650");
                (void)TryLiveFilterReload("stable-file-change");
            }

            const auto active=std::atomic_load_explicit(&PublishedFilterRules,
                std::memory_order_acquire);
            const auto excel=active ? (!active->baseNamesExcelPath.empty() ?
                active->baseNamesExcelPath : active->itemTypesExcelPath) :
                std::filesystem::path{};
            const bool watchItemTypes=active && !active->itemTypesExcelPath.empty();
            if(excel!=watchedBaseNameExcel || watchItemTypes!=watchedItemTypeCatalog) {
                watchedItemTypeCatalog=watchItemTypes;
                watchedBaseNameExcel=excel;
                weaponTableWatcher=FilterLiveReload::Watcher{};
                armorTableWatcher=FilterLiveReload::Watcher{};
                miscTableWatcher=FilterLiveReload::Watcher{};
                itemTypesTableWatcher=FilterLiveReload::Watcher{};
            }
            if(!excel.empty()) {
                const bool weaponsChanged=weaponTableWatcher.Observe(
                    ReadFilterFileStamp(excel/L"weapons.txt"),now,650);
                const bool armorChanged=armorTableWatcher.Observe(
                    ReadFilterFileStamp(excel/L"armor.txt"),now,650);
                const bool miscChanged=watchItemTypes && miscTableWatcher.Observe(
                    ReadFilterFileStamp(excel/L"misc.txt"),now,650);
                const bool typesChanged=watchItemTypes && itemTypesTableWatcher.Observe(
                    ReadFilterFileStamp(excel/L"itemtypes.txt"),now,650);
                if(weaponsChanged || armorChanged || miscChanged || typesChanged) {
                    Emit("LOOT_RELOAD_REQUEST version=" UNHOARDER_VERSION_STRING " trigger=stable-excel-change settleMs=650");
                    (void)TryLiveFilterReload("stable-excel-change");
                }
            }
        }

        DWORD pid{};
        (void)GetWindowThreadProcessId(GetForegroundWindow(),&pid);
        if(pid!=GetCurrentProcessId()) {
            lastReloadChord=false;
            continue;
        }

        // Manual configuration reload does not consume the game input.
        const bool reloadChord=(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0 &&
            (GetAsyncKeyState(VK_SHIFT)&0x8000)!=0 &&
            (GetAsyncKeyState(VK_F9)&0x8000)!=0;
        if(reloadChord && !lastReloadChord) {
            Emit("LOOT_RELOAD_REQUEST version=" UNHOARDER_VERSION_STRING " trigger=Ctrl+Shift+F9");
            (void)TryLiveFilterReload("Ctrl+Shift+F9");
            ruleFileWatcher.Resync(ReadFilterFileStamp(FilterConfigPath));
        }
        lastReloadChord=reloadChord;
    }
}
void RuntimeWorkerStart() noexcept {
    if(RuntimeWorker.joinable()) return;
    try {
        RuntimeWorker=std::jthread([](std::stop_token stop) noexcept {
            RuntimeWorkerLoop(stop);
        });
    } catch(const std::exception&) {
        Emit("LOOT_FILTER_INACTIVE runtime-worker-create-failed=1 liveReload=0");
    }
}
void RuntimeWorkerStop() noexcept {
    if(RuntimeWorker.joinable()) {
        RuntimeWorker.request_stop();
        RuntimeWorker.join();
    }
}



} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context) || context->abiVersion != D2RL_PLUGIN_ABI_VERSION)
        return false;
    Context = context;
    if(GetModuleHandleW(L"loot-filter.dll")!=nullptr ||
       GetModuleHandleW(L"d2rl-loot-filter.dll")!=nullptr) {
        context->LogError("LOOT_FILTER_REFUSED legacy loot-filter.dll is also loaded; remove the old production DLL before enabling UnHoarder");
        Context=nullptr;
        return false;
    }
    if(GetModuleHandleW(L"loot-filter-probe.dll")!=nullptr ||
       GetModuleHandleW(L"d2rl-loot-filter-probe.dll")!=nullptr) {
        context->LogError("LOOT_FILTER_REFUSED legacy loot-filter-probe DLL is also loaded; remove the old plugin before enabling UnHoarder");
        Context=nullptr;
        return false;
    }
    InWorldLifecycle = nullptr;
    InWorldJoinedListener = D2RL::Lifecycle::InvalidHandle;

    Base = context->exeBase;
    SoundArmed.store(false,std::memory_order_relaxed);
    SoundLoaderBase.store(0,std::memory_order_relaxed);
    SoundArmMs.store(0,std::memory_order_relaxed);
    SoundSeenRegistry.Clear();
    SoundRegistryEpoch.store(1);
    SoundSeenTotal.store(0);SoundBaselineTotal.store(0);
    SoundQualifiedNew.store(0);SoundQueued.store(0);
    SoundQueueRejected.store(0);SoundPlayed.store(0);SoundUnknown.store(0);
    SoundCancelled.store(0);SoundCacheContention.store(0);SoundCacheFull.store(0);
    SoundAlreadySeen.store(0);SoundObservedAlt.store(0);
    SoundPickupPollPending.store(false);
    SoundPickupPolls.store(0);SoundPickupPollSuccess.store(0);
    SoundPickupResets.store(0);SoundPickupBusy.store(0);
    SoundPickupPollRejected.store(0);
    SoundInventory=nullptr;
    SoundInventoryItems=nullptr;
    SoundThreads=nullptr;
    const D2RL::ThreadService* soundThreads{};
    if(context->QueryService(&soundThreads)==D2RL::ServiceQueryResult::Success &&
       D2RL::HasThreadServiceField(soundThreads,D2RL::ThreadServiceRequiredSize) &&
       soundThreads->runOnGameThread)SoundThreads=soundThreads;
    {
        auto* module=GetModuleHandleW(L"D2RLoader.exe");
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        const auto loaderBase=reinterpret_cast<std::uintptr_t>(module);
        const bool peOk=module &&
            SoundMemoryRead(loaderBase,&dos,sizeof(dos)) &&
            dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>=64 &&
            dos.e_lfanew<=4096 &&
            SoundMemoryRead(loaderBase+static_cast<std::uintptr_t>(dos.e_lfanew),
                &nt,sizeof(nt)) && nt.Signature==IMAGE_NT_SIGNATURE &&
            nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        char line[310]{};
        std::snprintf(line,sizeof(line),
            "LOOT_COMPAT_START version=" UNHOARDER_VERSION_STRING " targetBuild=93847 "
            "sourceMarker=unhoarder-prod-v1 loaderImage=%u peOk=%u stamp=0x%X imageSize=0x%X "
            "threadService=%u quantityPolicy=fail-closed soundPolicy=fail-closed",
            module?1U:0U,peOk?1U:0U,
            peOk?unsigned(nt.FileHeader.TimeDateStamp):0U,
            peOk?unsigned(nt.OptionalHeader.SizeOfImage):0U,
            SoundThreads?1U:0U);
        Emit(line);
        ResetMinimapTracking();
    }

    ActiveGeometryMode.store(GeometryMode::Off, std::memory_order_relaxed);
    SharedLabelPaintCompatRoute.store(TooltipCompatRoute::None,std::memory_order_relaxed);
    GlyphRendererCompatRoute.store(TooltipCompatRoute::None,std::memory_order_relaxed);
    TooltipCompatCommunication=nullptr;
    ForeignPaintCompatHandle=TooltipCompat::InvalidRegistrationHandle;
    ForeignGlyphCompatHandle=TooltipCompat::InvalidRegistrationHandle;
    std::atomic_store_explicit(
        &TooltipPaintSubscribers,
        std::shared_ptr<const std::vector<TooltipPaintSubscriber>>{},
        std::memory_order_release);
    std::atomic_store_explicit(
        &TooltipGlyphSubscribers,
        std::shared_ptr<const std::vector<TooltipGlyphSubscriber>>{},
        std::memory_order_release);
    BackgroundPaintHookInstalled.store(false,std::memory_order_relaxed);
    OriginalSharedLabelPaint=nullptr;
    GroundIdentities.fill({});
    GroundPropertyLiveReads.store(0,std::memory_order_relaxed);
    GroundPropertyLiveUnknown.store(0,std::memory_order_relaxed);
    BackgroundTintArmed.store(false,std::memory_order_relaxed);
    HideGroundArmed.store(false,std::memory_order_relaxed);
    OriginalInnerNameWriter=nullptr;
    InnerNameHookInstalled.store(false,std::memory_order_relaxed);
    GeometryTotalCalls.store(0,std::memory_order_relaxed);
    GeometrySourceCalls.store(0,std::memory_order_relaxed);
    GeometryValidPairs.store(0,std::memory_order_relaxed);
    GeometryIdMismatches.store(0,std::memory_order_relaxed);
    GeometryLookups.store(0,std::memory_order_relaxed);
    GeometryNoRule.store(0,std::memory_order_relaxed);
    GeometryReadFailures.store(0,std::memory_order_relaxed);
    GeometryNoTerminator.store(0,std::memory_order_relaxed);
    GeometryWrites.store(0,std::memory_order_relaxed);
    GeometryObserved.store(0,std::memory_order_relaxed);
    FormatterHookInstalled.store(false, std::memory_order_relaxed);
    FormatterCalls.store(0,std::memory_order_relaxed);
    std::atomic_store_explicit(&PublishedFilterRules,
        std::shared_ptr<const FilterRuleTable>{},std::memory_order_release);
    FilterGeneration.store(0);
    FilterLookups.store(0);
    FilterMatches.store(0);
    FilterNoRule.store(0);
    FilterWrites.store(0);
    FilterGuardFailures.store(0);
    FilterReloadSucceeded.store(0);
    FilterReloadRefused.store(0);
    FilterLiveReloadAvailable.store(false,std::memory_order_release);
    if (ResolveFilterConfigPath()) {
        // Parsing happens before any ground-label feature hook is installed.
        // Missing/invalid JSON leaves the filter unarmed; no fallback rules
        // are silently created next to the user's DLL.
        (void)ReloadFilterRules();
        const auto path=std::string("LOOT_RULES_PATH '")+FilterPathUtf8()+"'";
        Emit(path.c_str());
    } else Emit("LOOT_RULES_WARNING could-not-resolve-own-DLL-directory");
    if (!DetermineImageSize()) {
        context->LogError("LOOT_FILTER_REFUSED invalid main D2R PE image");
        return false;
    }
    const char* build = D2RL::GetBuildName(context);
    char msg[310]{};
    std::snprintf(msg, sizeof(msg),
        "LOOT_FILTER_READY version=" UNHOARDER_VERSION_STRING " build=%s expectedBuild=93847 config=automatic",
        build ? build : "unknown");
    Emit(msg);
    if (!build || std::string_view(build) != "93847") {
        context->LogWarn("LOOT_FILTER_INACTIVE unexpected build, no native hook");
        return true;
    }
    if (!context->CheckExpectedBytes(GetItemCodeRva, ExpectedGetItemCode.data(),
            static_cast<std::uint32_t>(ExpectedGetItemCode.size()))) {
        context->LogWarn("LOOT_FILTER_INACTIVE helper fingerprint mismatch or owned bridge; no hook chaining");
        return true;
    }
    if (!context->InstallInlineHook(GetItemCodeRva, ExpectedGetItemCode.data(),
            static_cast<std::uint32_t>(ExpectedGetItemCode.size()),
            HookGetItemCode, &OriginalGetItemCode) || !OriginalGetItemCode) {
        context->LogWarn("LOOT_FILTER_INACTIVE hook registration refused, no fallback patches");
        return true;
    }
    HookInstalled.store(true, std::memory_order_release);
    PublishTooltipCompatService();
    (void)InstallStandaloneAutomapProjection();
    // Reuse the qualified native filter pipeline automatically once the
    // complete JSON ruleset is published and the item-code reader is ready.
    if (std::atomic_load_explicit(&PublishedFilterRules,
            std::memory_order_acquire))
        (void)ActivateConfiguredFilter(true);
    else Emit("LOOT_FILTER_AUTO_INACTIVE reason=json-absent-or-invalid no-ground-label-feature-hooks=1");
    RegisterInWorldLifecycle();
    StartSoundInventoryObserver();
    InstallNativePickupGuard();
    RuntimeWorkerStart();

    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    RuntimeWorkerStop();
    DetachForeignTooltipCompatRoutes();
    AutomapProjectionArmed.store(false,std::memory_order_release);
    MinimapOverlayRenderer::Shutdown();
    AutomapProjectionHookInstalled.store(false,std::memory_order_release);
    FilterLiveReloadAvailable.store(false,std::memory_order_release);
    NativePickupGuardQualified.store(false,std::memory_order_release);
    GroundQuantityReader.store(nullptr,std::memory_order_release);
    HideGroundArmed.store(false,std::memory_order_release);
    // Stop background scheduling before allowing loader-owned service pointers
    // or the plugin context to become invalid. No new inventory callback is
    // queued after this join; loader owns cancellation of already-queued work.
    SoundArmed.store(false,std::memory_order_release);
    if (SoundPickupPollWorker.joinable()) {
        SoundPickupPollWorker.request_stop();
        SoundPickupPollWorker.join();
    }
    SoundPickupPollPending.store(false,std::memory_order_release);
    SoundInventory=nullptr;
    SoundInventoryItems=nullptr;
    if (InWorldLifecycle && InWorldJoinedListener
        != D2RL::Lifecycle::InvalidHandle
        && InWorldLifecycle->unregisterGameplayEventListener
        && Context) {
        (void)InWorldLifecycle->unregisterGameplayEventListener(
            Context, InWorldJoinedListener);
    }
    InWorldLifecycle = nullptr;
    InWorldJoinedListener = D2RL::Lifecycle::InvalidHandle;
    SoundArmed.store(false,std::memory_order_release);
    SoundLoaderBase.store(0,std::memory_order_release);
    ActiveGeometryMode.store(GeometryMode::Off, std::memory_order_release);
    BackgroundTintArmed.store(false,std::memory_order_release);

    std::atomic_store_explicit(&PublishedFilterRules,
        std::shared_ptr<const FilterRuleTable>{},std::memory_order_release);
    FilterConfigPath.clear();
    HookInstalled.store(false, std::memory_order_release);
    SharedLabelPaintCompatRoute.store(TooltipCompatRoute::None,std::memory_order_release);
    GlyphRendererCompatRoute.store(TooltipCompatRoute::None,std::memory_order_release);
    {
        std::scoped_lock lock(TooltipCompatMutex);
        std::atomic_store_explicit(
            &TooltipPaintSubscribers,
            std::shared_ptr<const std::vector<TooltipPaintSubscriber>>{},
            std::memory_order_release);
        std::atomic_store_explicit(
            &TooltipGlyphSubscribers,
            std::shared_ptr<const std::vector<TooltipGlyphSubscriber>>{},
            std::memory_order_release);
    }
    OriginalGetItemCode = nullptr;
    // The loader owns detour removal. Do not null out native trampolines
    // while other thread callbacks could still be forwarding through them.

    SoundThreads = nullptr;
    TooltipCompatCommunication = nullptr;
    Context = nullptr;
    Base = 0;
    ImageSize = 0;
}

} // namespace SoE::LootFilter
