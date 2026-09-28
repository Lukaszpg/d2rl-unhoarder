#include <D2RLPlugin/api.h>
#include <D2RLPlugin/item.h>
#include <D2RLPlugin/inventory.h>
#include <D2RLPlugin/lifecycle_events.h>
#include "soe_in_world_label_api.hpp"
#include "soe_in_world_label_style_api.hpp"
#include "soe_in_world_render_scope_api.hpp"
#include "hover_label_style.hpp"
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
#include "native_row_live_display_match.hpp"
#include "ground_sound_registry.hpp"
#include "named_sound_loader_identity.hpp"
#include "native_row_string_layout.hpp"
#include "native_row_append_match.hpp"
#include "native_row_bg_policy.hpp"
#include "native_row_bg_live_policy.hpp"
#include "native_row_font_color_policy.hpp"
// Suite-vendored PluginSDK-v4 declares ThreadServiceV1 and requires explicit service ID/version.
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
constexpr std::uint32_t DivineCode =
    static_cast<std::uint32_t>('d') |
    (static_cast<std::uint32_t>('i') << 8U) |
    (static_cast<std::uint32_t>('v') << 16U) |
    (static_cast<std::uint32_t>('o') << 24U);
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
constexpr std::size_t MaximumFilterFileBytes = 64 * 1024;
constexpr std::size_t MaximumFilterRules = 256;
constexpr std::size_t MaximumFilterNameBytes = 79; // measured in BYTES (no NUL)
// 0.1.52 verified on D2R 93847: per-ground-label 4xfloat RGBA at
// record+0x14. 0x1517AE6 / 0x1519E36 passes record+0x14 as third arg
// to 0x1FA8E0, which forwards that pointer as fifth argument to
// 0x657B90. 0x657B90 reads offsets 0,4,8,12 and converts them to RGBA
// bytes before drawing the filled rectangle, then 0x1FA8E0 draws text.
// The native baseline captured by 0.1.23 is (0,0,0,0.6f).
constexpr std::uintptr_t GroundColorOffset = 0x14;
constexpr std::array<float,4> OriginalGroundBackground{{0.0f,0.0f,0.0f,0.6f}};
constexpr std::array<float,4> PurpleDivineBackground{{0.45f,0.08f,0.55f,0.82f}};
std::atomic_bool BackgroundTintArmed{};
std::atomic_bool HideGroundArmed{};
std::atomic<std::uint64_t> HiddenGroundPaints{};
std::atomic<std::uint64_t> HiddenGroundInteractionPainterSkips{};
std::atomic<std::uint32_t> HiddenGroundLastPainterSkipId{};
std::atomic<std::uint32_t> HiddenGroundLastPainterSkipCode{};
std::atomic<ULONGLONG> HiddenGroundLastPainterSkipMs{};
std::atomic<std::uint64_t> HiddenHoverRowsSuppressed{};
std::atomic_bool BackgroundTintEverArmed{};
std::atomic<std::uint64_t> BackgroundQualified{};
std::atomic<std::uint64_t> BackgroundMatched{};
std::atomic<std::uint64_t> BackgroundNoMatch{};
std::atomic<std::uint64_t> BackgroundGuardFailures{};
std::atomic<std::uint64_t> BackgroundWrites{};
std::atomic<std::uint64_t> BackgroundRestores{};

// Observe the NATIVE color at the shared paint boundary and optionally
// forward a per-item RGBA pointer into the original paint helper.
// The native label records are NEVER changed by the 0.1.52 tint test.
using SharedLabelPaintFn = void(__fastcall*)(void*,void*,void*) noexcept;
SharedLabelPaintFn OriginalSharedLabelPaint{};
std::atomic_bool BackgroundPaintHookInstalled{};
std::atomic_bool BackgroundPaintObserveArmed{};
std::atomic<std::uintptr_t> LastDivineTintRecord{};
std::atomic<std::uintptr_t> LastMapTintRecord{};
// Unit IDs were paired with native item code through the verified formatter.
// This is an intentionally single-Divine proof of concept, not a production cache.
std::atomic<std::uint32_t> LastDivineTintUnitId{};
std::atomic<std::uint32_t> LastMapTintUnitId{};
// A bounded, ephemeral identity bridge: the formatter can inspect native unit
// and code; the renderer has the unit ID but no native unit pointer. A label
// may be rendered from a *different* record address. No heap allocation or
// blocking lock in the paint hook; stale IDs are rejected after three seconds.
constexpr std::size_t IdentitySlots = 512;
constexpr std::size_t IdentityProbeSlots = 8;
constexpr ULONGLONG IdentityTtlMs = 3000;
struct VerifiedGroundIdentity {
    std::uint32_t unitId{};
    std::uint32_t code{};
    std::uint32_t classId{};
    ULONGLONG seenMs{};
    std::array<char,80> visibleName{};
    std::uint32_t quantity{}; // sampled from this exact formatter item
    // Only scalar facts copied from the same verified formatter unit;
    // never retain native pointers across paint calls.
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
std::atomic<std::uint64_t> GroundIdentityUpdates{};
std::atomic<std::uint64_t> GroundIdentitySkips{};
std::atomic<std::uint64_t> BackgroundRuleForwarded{};
std::atomic<std::uint64_t> BackgroundRuleNoColor{};
std::atomic<std::uint64_t> BackgroundRuleNoIdentity{};
std::atomic<std::uint64_t> BackgroundPaintForwardedPurple{}; // historical diagnostic alias

std::atomic<std::uint64_t> BackgroundPaintIdQualified{};
std::atomic<std::uint64_t> BackgroundPaintIdRejected{};
std::atomic<std::uint64_t> BackgroundPaintNameRejected{};
std::atomic<std::uint64_t> BackgroundPaintColorRejected{};
std::atomic<std::uint64_t> BackgroundPaintCalls{};
std::atomic<std::uint64_t> BackgroundPaintGroundCalls{};
std::atomic<std::uint64_t> BackgroundPaintNeighborCalls{};
std::atomic<std::uint64_t> BackgroundPaintDivineCalls{};
std::atomic<std::uint64_t> BackgroundPaintDivinePurple{};
std::atomic<std::uint64_t> BackgroundPaintDivineBlack{};
std::atomic<std::uint64_t> BackgroundPaintMapCalls{};
std::atomic<std::uint64_t> BackgroundPaintMapPurple{};
std::atomic<std::uint64_t> BackgroundPaintMapBlack{};
std::atomic<std::uint64_t> BackgroundPaintOtherCalls{};
std::atomic<std::uint64_t> BackgroundPaintReadFailures{};
std::atomic<std::uint64_t> BackgroundPaintLockContention{};
struct BackgroundPaintSample {
    std::uintptr_t rect{};
    std::uintptr_t text{};
    std::uintptr_t color{};
    std::array<float,4> rgba{}; // native input, BEFORE optional override
    std::array<char,64> name{};
    std::uint32_t recordId{};
    bool forwardedPurple{};
    std::uint64_t hits{};
    bool valid{};
};
std::mutex BackgroundPaintSampleMutex;
BackgroundPaintSample BackgroundPaintLastDivine{};
BackgroundPaintSample BackgroundPaintLastMap{};
BackgroundPaintSample BackgroundPaintLastOther{};

// 0.1.52: observe native glyph RGBA at the ALREADY VERIFIED shared paint
// boundary. Do not hook 0x902E20 with a three-argument C++ function: the
// native caller passes a packed 128-bit render value in VOLATILE XMM3,
// which the 0.1.32 forwarding thunk did not preserve (invisible glyphs).
constexpr std::uintptr_t GroundGlyphColorOffset=0xAC; // record+0xA4 style + 8
constexpr std::array<std::uint32_t,4> NativeDivineGlyphColor{240,240,240,255};
constexpr std::array<std::uint32_t,4> ProbeCyanGlyphColor{60,225,255,255};
std::atomic_bool GroundTextObserveArmed{};
std::atomic_bool GroundTextCyanArmed{};
std::atomic<std::uint64_t> GroundTextSamples{};
std::atomic<std::uint64_t> GroundTextDivine{};
std::atomic<std::uint64_t> GroundTextMap{};
std::atomic<std::uint64_t> GroundTextReadFailures{};
std::atomic<std::uint64_t> GroundTextUnverified{};
std::atomic<std::uint64_t> GroundTextCyanWrites{};
std::atomic<std::uint64_t> GroundTextCyanRestores{};
std::atomic<std::uint64_t> GroundTextCyanAlreadyPresent{};
std::atomic<std::uint64_t> GroundTextCyanAfterOriginal{};
std::atomic<std::uint64_t> GroundTextNativeAfterOriginal{};
std::atomic<std::uint64_t> GroundTextCyanAfterReadFailures{};
std::atomic<std::uint64_t> GroundTextCyanColorRejects{};
std::atomic<std::uint64_t> GroundTextCyanWriteGuards{};
std::atomic<std::uint64_t> GroundTextCyanContended{};
std::mutex GroundTextCyanWriteMutex;
struct GroundGlyphColorSample {
    std::array<std::uint32_t,4> rgba{};
    std::uint32_t unitId{};
    std::uint64_t hits{};
    bool valid{};
};
std::mutex GroundGlyphColorSamplesMutex;
GroundGlyphColorSample GroundDivineGlyphSample{};
GroundGlyphColorSample GroundMapGlyphSample{};

bool WritableRange(void* address,std::size_t bytes) noexcept {
    if (!address || !bytes) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(address,&memory,sizeof(memory)) ||
        memory.State!=MEM_COMMIT ||
        (memory.Protect & (PAGE_NOACCESS|PAGE_GUARD))) return false;
    const auto protection=memory.Protect & 0xFFU;
    if (protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY &&
        protection!=PAGE_EXECUTE_READWRITE &&
        protection!=PAGE_EXECUTE_WRITECOPY) return false;
    const auto start=reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const auto target=reinterpret_cast<std::uintptr_t>(address);
    return start<=target && memory.RegionSize<=UINTPTR_MAX-start &&
        start+memory.RegionSize>=target &&
        start+memory.RegionSize-target>=bytes;
}

// Declared before the ground-text status reporter; defined below.
void Emit(const char* message) noexcept;
// Declared before 0.2.39 ground evidence drainers; defined with other text helpers.
void CodeText(std::uint32_t code, char (&out)[5]) noexcept;
bool PrintableItemCode(std::uint32_t code) noexcept;

void ReportGroundGlyphStatus() noexcept {
    char msg[620]{};
    std::snprintf(msg,sizeof(msg),
        "LOOT_GROUND_TEXT_STYLE_STATUS version=1.0.0 observing=%u cyanArmed=%u sharedPaintHook=%u samples=%llu divine=%llu map=%llu readFailures=%llu unverified=%llu cyanWrites=%llu restored=%llu retainedCyan=%llu afterDrawCyan=%llu afterDrawNative=%llu afterDrawReadFailures=%llu nativeColorRejected=%llu guards=%llu contended=%llu textRendererHook=0 glyphRGBA=record+0xAC recordWrites=guarded-persistent-style-until-off itemWrites=0",
        GroundTextObserveArmed.load()?1U:0U,GroundTextCyanArmed.load()?1U:0U,
        BackgroundPaintHookInstalled.load()?1U:0U,
        static_cast<unsigned long long>(GroundTextSamples.load()),
        static_cast<unsigned long long>(GroundTextDivine.load()),
        static_cast<unsigned long long>(GroundTextMap.load()),
        static_cast<unsigned long long>(GroundTextReadFailures.load()),
        static_cast<unsigned long long>(GroundTextUnverified.load()),
        static_cast<unsigned long long>(GroundTextCyanWrites.load()),
        static_cast<unsigned long long>(GroundTextCyanRestores.load()),
        static_cast<unsigned long long>(GroundTextCyanAlreadyPresent.load()),
        static_cast<unsigned long long>(GroundTextCyanAfterOriginal.load()),
        static_cast<unsigned long long>(GroundTextNativeAfterOriginal.load()),
        static_cast<unsigned long long>(GroundTextCyanAfterReadFailures.load()),
        static_cast<unsigned long long>(GroundTextCyanColorRejects.load()),
        static_cast<unsigned long long>(GroundTextCyanWriteGuards.load()),
        static_cast<unsigned long long>(GroundTextCyanContended.load()));
    Emit(msg);
    std::lock_guard lock(GroundGlyphColorSamplesMutex);
    const auto report=[](const char* tag,const GroundGlyphColorSample& sample) noexcept {
        if (!sample.valid) return;
        char line[270]{};
        std::snprintf(line,sizeof(line),
            "LOOT_GROUND_TEXT_STYLE type=%s hits=%llu unitId=%u nativeRGBA=%u,%u,%u,%u recordOffset=+0xAC",
            tag,static_cast<unsigned long long>(sample.hits),sample.unitId,
            sample.rgba[0],sample.rgba[1],sample.rgba[2],sample.rgba[3]);
        Emit(line);
    };
    report("divine",GroundDivineGlyphSample);
    report("map",GroundMapGlyphSample);
}



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
// The bridge may chain through SoE's StatReadBus: require the target to belong
// to the known SoE or D2RCore module, not an arbitrary executable pointer.
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
            "LOOT_COMPAT_QUANTITY version=1.0.0 build=93847 readerRva=0x%llX "
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
        Context->LogInfo("LOOT_COMPAT_QUANTITY_BRIDGE_ACCEPTED version=1.0.0 slotRva=0x3E2A218 fingerprint=exact targetAdmission=pending ownerRestriction=D2RCore-or-SoE");
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
        if(loader131Bridge) Context->LogWarn("LOOT_COMPAT_QUANTITY_OWNER version=1.0.0 result=unresolved quantity=disabled");
        return;
    }
    if(loader131Bridge) {
        char ownerLine[220]{};
        const char* const ownerKind=owner==GetModuleHandleW(L"D2RCore.dll")?"D2RCore":
            owner==GetModuleHandleW(L"d2rl-soe.dll")?"d2rl-soe":"other";
        std::snprintf(ownerLine,sizeof(ownerLine),
            "LOOT_COMPAT_QUANTITY_OWNER version=1.0.0 owner=%s executePage=1 quantity=%s",
            ownerKind,std::string_view(ownerKind)=="other"?"disabled":"admitted");
        Context->LogInfo(ownerLine);
    }
    if (owner!=GetModuleHandleW(L"D2RCore.dll") &&
        owner!=GetModuleHandleW(L"d2rl-soe.dll")) {
        Context->LogWarn("LOOT_QUANTITY_UNAVAILABLE reader-owner-unrecognized fallback=vanilla");
        return;
    }
    GroundQuantityReader.store(
        reinterpret_cast<GroundQuantityReaderFn>(base+GroundQuantityReaderRva),
        std::memory_order_release);
    Context->LogInfo("LOOT_QUANTITY_READY version=1.0.0 stat=quantity/70 source=qualified-loader-bridge mode=auto ground-only unitWrites=0");
}

std::uint32_t GroundStackQuantity(const void* borrowedNativeUnit) noexcept {
    const auto getter=GroundQuantityReader.load(std::memory_order_acquire);
    if (!getter || !borrowedNativeUnit) return 0;
    // SoE V2 guarantees a borrowed live native TYPE_ITEM during this callback.
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
// fingerprint validation; no diagnostic sample/capture buffers remain.
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
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=1.0.0 reason=no-image-or-context hooks=0 mapSenseDependency=0");
        return false;
    }
    if(GetModuleHandleW(L"d2rl-ruffneckk-mapsense.dll")!=nullptr) {
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=1.0.0 reason=mapsense-loaded-shared-rendezvous hooks=0 coexistence=preserved");
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
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=1.0.0 reason=native-contract-fingerprint hooks=0");
        return false;
    }
    ProjectClientToAutomap=reinterpret_cast<ProjectClientToAutomapFn>(Base+ProjectClientToAutomapRva);
    GetLocalDataContext=reinterpret_cast<GetLocalDataContextFn>(Base+GetLocalDataContextRva);
    GetLocalPlayer=reinterpret_cast<GetLocalPlayerFn>(Base+GetLocalPlayerRva);
    if(!Context->InstallInlineHook(AutomapRenderUnitRva,ExpectedAutomapRenderUnit.data(),
            static_cast<std::uint32_t>(ExpectedAutomapRenderUnit.size()),
            HookAutomapRenderUnit,&OriginalAutomapRenderUnit) || !OriginalAutomapRenderUnit) {
        ProjectClientToAutomap=nullptr;GetLocalDataContext=nullptr;GetLocalPlayer=nullptr;
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=1.0.0 reason=loader-hook-registration hooks=0");
        return false;
    }
    AutomapProjectionHookInstalled.store(true,std::memory_order_release);
    Emit("LOOT_MINIMAP_PROJECTION_READY version=1.0.0 hook=D2R+0xD76E0 project=D2R+0xD4910 worldPosition=qualified-static-path renderer=standalone mapSenseDependency=0");
    return true;
}

void __cdecl LogMinimapRendererDiagnostic(const char* message) noexcept { Emit(message); }

bool ConfigureNativeAutomapVisibilityGate() noexcept {
    MinimapOverlayRenderer::SetAutomapVisibilityTable(nullptr);
    if(!Context || !Base) {
        Emit("LOOT_MINIMAP_AUTOMAP_GATE_REFUSED version=1.0.0 reason=no-image-or-context drawing=0");
        return false;
    }
    const auto check=[&](std::uintptr_t rva,const auto& expected) {
        return Context->CheckExpectedBytes(rva,expected.data(),
            static_cast<std::uint32_t>(expected.size()));
    };
    if(!check(NativeUiOpenStateWitnessRva,ExpectedNativeUiOpenStateWitness) ||
       !check(NativeUiCloseStateWitnessRva,ExpectedNativeUiCloseStateWitness) ||
       !check(NativeUiToggleStateWitnessRva,ExpectedNativeUiToggleStateWitness)) {
        Emit("LOOT_MINIMAP_AUTOMAP_GATE_REFUSED version=1.0.0 reason=ui-state-fingerprint drawing=0");
        return false;
    }
    MinimapOverlayRenderer::SetAutomapVisibilityTable(
        reinterpret_cast<const volatile std::uint8_t*>(Base+NativeUiStateTableRva));
    Emit("LOOT_MINIMAP_AUTOMAP_GATE_READY version=1.0.0 table=D2R+0x2A2ADA0 automapState=10 readOnly=1");
    return true;
}

bool InitializeMinimapMarkerRenderer() noexcept {
    HMODULE self{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&InitializeMinimapMarkerRenderer),&self) || !self) {
        Emit("LOOT_MINIMAP_RENDERER_REFUSED version=1.0.0 backend=none reason=self-module-unresolved");
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

// 0.1.99: backend selection occurs after plugin startup so either load order
// is safe. SoE owns both native entry + merge when available; no double detour.
enum class InWorldBackend : int {
    Pending = 0, SoEInterop = 1, StandaloneIdentity = 2, Blocked = 3
};
constexpr char LootFilterInteropOwner[] = "loot-filter";
constexpr std::uintptr_t InWorldFormatterRva = 0xC0420;
constexpr std::array<std::uint8_t, 16> ExpectedInWorldFormatter{{
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,
    0x24,0x18,0x48,0x89,0x7C,0x24,0x20,0x55
}};
using InWorldFormatterFn = std::uintptr_t(__fastcall*)(void*) noexcept;
InWorldFormatterFn OriginalInWorldFormatter{};
std::atomic<InWorldBackend> InWorldMode{InWorldBackend::Pending};
std::atomic<std::uint64_t> InWorldCalls{};
std::atomic<std::uint64_t> InWorldDivineCalls{};
std::atomic<std::uint64_t> InWorldTextCalls{};
std::mutex InWorldSampleMutex{};
struct InWorldSample final {
    std::int32_t type{-1};
    std::uint32_t classId{};
    std::uint32_t unitId{};
    std::uint32_t code{};
    std::uint32_t sourceLength{};
    std::array<std::uint8_t, 16> firstBytes{};
};
InWorldSample LastInWorldSample{};
const D2RL::LifecycleServiceV1* InWorldLifecycle{};
D2RL::Lifecycle::ListenerHandle InWorldJoinedListener{D2RL::Lifecycle::InvalidHandle};
using GetSoEInteropFn = const SoE::Interop::InWorldLabelApiV1*(__cdecl*)() noexcept;

using GetSoEStyleFn = const SoE::Interop::InWorldLabelStyleApiV2*(__cdecl*)() noexcept;
using GetSoERenderScopeFn = const SoE::Interop::InWorldRenderScopeApiV3*(__cdecl*)() noexcept;
std::atomic<const SoE::Interop::InWorldRenderScopeApiV3*> InWorldRenderScopeApi{};
// Opt-in read-only native row observation. No row or rectangle hook at startup.
void ResetNativeRowLiveSession() noexcept;
void EnableAutomaticNativeHover() noexcept;

std::atomic_bool InWorldStyleAttached{false};
std::atomic<std::uint64_t> InWorldStyleCalls{};
std::atomic<std::uint64_t> InWorldStyleWrites{};
std::atomic<std::uint64_t> InWorldStyleNoRule{};
std::atomic<std::uint64_t> InWorldStyleUnsupportedColor{};
std::atomic<std::uint64_t> InWorldStyleGuarded{};
// These observers are used by SoE V1 and the standalone identity fallback.
// Their implementations are below; forward declarations are necessary because
// production cleanup retained the call sites but dropped the definitions.
void ObserveGroundSoundIdentity(std::uint32_t unitId,
    std::uint32_t rawCode, bool hiddenHover,
    const void* nativeUnit,std::uint32_t knownClassId) noexcept;
void ObserveNativeRowLiveLabel(std::int32_t type,
    std::uint32_t classId, std::uint32_t unitId,
    std::uint32_t rawCode, const void* nativeUnit, const char* source,
    std::uint32_t sourceLength) noexcept;
void ObserveNativeRowLiveReplacement(std::uint32_t classId,
    std::uint32_t unitId, std::uint32_t rawCode,
    std::string_view source, std::string_view rendered) noexcept;

void RuntimeWorkerStart() noexcept;
void RuntimeWorkerStop() noexcept;
void RecordInWorldSample(std::int32_t type, std::uint32_t classId,
    std::uint32_t unitId, std::uint32_t code,
    const void* nativeUnit,
    const char* source, std::uint32_t sourceLength) noexcept {
    if(type==4 && source && sourceLength && sourceLength<=255)
        ObserveMinimapItemPosition(nativeUnit,code,unitId,classId);
    ObserveGroundSoundIdentity(unitId,code,true,nativeUnit,classId);
    ObserveNativeRowLiveLabel(type,classId,unitId,code,nativeUnit,source,sourceLength);
}

void __cdecl OnSoEInWorldLabel(
    const SoE::Interop::InWorldLabelEventV1* event, void*) noexcept {
    if (!event || event->structSize < sizeof(*event) ||
        event->unitType != 4 || !event->nativeUnit || !event->source ||
        !event->sourceLength || event->sourceLength > 255U) return;
    const auto code = OriginalGetItemCode ? OriginalGetItemCode(
        const_cast<void*>(event->nativeUnit)) : 0U;
    RecordInWorldSample(event->unitType,event->classId,event->unitId,
        code,event->nativeUnit,event->source,event->sourceLength);
}

// Definitions occur later; no parser or other file I/O on native hover path.
bool __cdecl OnSoEInWorldStyle(const SoE::Interop::InWorldLabelEventV1* event,
    char* replacement, std::uint32_t capacity, void*) noexcept {
    InWorldStyleCalls.fetch_add(1, std::memory_order_relaxed);
    if (!event || event->structSize < sizeof(*event) || event->unitType != 4 ||
        !event->nativeUnit || !event->source || !event->sourceLength ||
        event->sourceLength > 255U || !replacement || capacity == 0U ||
        !OriginalGetItemCode || !HookInstalled.load(std::memory_order_acquire) ||
        ActiveGeometryMode.load(std::memory_order_acquire) != GeometryMode::Rules) {
        InWorldStyleGuarded.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto snapshot = std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (!snapshot) return false;
    const auto code=CanonicalItemCode(OriginalGetItemCode(
        const_cast<void*>(event->nativeUnit)));
    const auto observed=GroundRuleItem(code,event->nativeUnit,
        snapshot.get(),event->unitId,
        GroundPropertyLive::Purpose::VerifiedLabel);
    GroundRuleDecision resolvedRule{};
    const auto* rule=ResolveGroundRule(snapshot.get(),observed,resolvedRule) ?
        &resolvedRule : nullptr;
    UpdateMinimapProjectionIconRule(event->unitId,code,rule);
    // Quantity is independent of JSON rules: e.g. an unfiltered stack of
    // consumables still gets "3x ". Stat 0/1 retains vanilla display.
    const auto quantity=GroundStackQuantity(event->nativeUnit);
    const char palette=rule && rule->hasTextColor ?
        HoverStyle::PaletteSelector(rule->textColor):'\0';
    if (rule && rule->hasTextColor && !palette)
        InWorldStyleUnsupportedColor.fetch_add(1,std::memory_order_relaxed);
    const auto source=std::string_view(event->source,event->sourceLength);
    const auto name=rule && rule->hasName ?
        std::string_view(rule->name.data(),rule->bytes-1U):std::string_view{};
    if (!GroundQuantity::BuildHover(source,name,rule && rule->hasName,
            palette,quantity,replacement,capacity)) {
        if (rule || quantity>1)
            InWorldStyleGuarded.fetch_add(1,std::memory_order_relaxed);
        else InWorldStyleNoRule.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    // V1 observed the unmodified text before this V2 transformer ran.
    // The native styled-text row will instead contain these returned bytes;
    // update only the SAME scoped item's display witness, retaining the
    // original V1 source separately for item/event identity qualification.
    if (const auto* end=static_cast<const char*>(
            std::memchr(replacement,'\0',capacity));end && end>replacement)
        ObserveNativeRowLiveReplacement(event->classId,event->unitId,
            code,source,std::string_view(replacement,
                static_cast<std::size_t>(end-replacement)));
    InWorldStyleWrites.fetch_add(1,std::memory_order_relaxed);
    return true;
}

std::uintptr_t __fastcall StandaloneInWorldIdentityHook(void* unit) noexcept {
    const auto original = OriginalInWorldFormatter;
    if (!original) return 0;
    // Identity-only fallback: no relay hijack, no writes. Full hidden-label
    // text in the independent backend still requires a separately qualified
    // loader bridge. This hook never installs when SoE is loaded.
    if (unit && OriginalGetItemCode) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(unit, &mbi, sizeof(mbi)) != 0
            && mbi.State == MEM_COMMIT
            && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            const auto start = reinterpret_cast<std::uintptr_t>(unit);
            const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress)
                + mbi.RegionSize;
            if (start < regionEnd && regionEnd - start >= 12) {
                std::uint32_t type{}, classId{}, unitId{};
                std::memcpy(&type, unit, 4);
                std::memcpy(&classId, static_cast<const char*>(unit) + 4, 4);
                std::memcpy(&unitId, static_cast<const char*>(unit) + 8, 4);
                if (type == 4) RecordInWorldSample(4, classId, unitId,
                    OriginalGetItemCode(unit),unit,nullptr, 0);
            }
        }
    }
    return original(unit);
}

void TryAttachInWorldBackend() noexcept {
    if (!Context || InWorldMode.load(std::memory_order_acquire)
        != InWorldBackend::Pending) return;
    if (!D2RL::GetBuildName(Context)
        || std::string_view(D2RL::GetBuildName(Context)) != "93847") {
        InWorldMode.store(InWorldBackend::Blocked, std::memory_order_release);
        Emit("LOOT_INWORLD_BLOCKED unqualified-build nativeHooks=0");
        return;
    }
    if (auto* soe = GetModuleHandleW(L"d2rl-soe.dll")) {
        const auto getApi = reinterpret_cast<GetSoEInteropFn>(
            GetProcAddress(soe, SoE::Interop::InWorldLabelExportName));
        const auto* api = getApi ? getApi() : nullptr;
        if (api && api->structSize >= sizeof(SoE::Interop::InWorldLabelApiV1)
            && api->abiVersion == SoE::Interop::InWorldLabelAbiV1
            && api->isReady && api->registerObserver && api->unregisterObserver
            && api->isReady()
            && api->registerObserver(LootFilterInteropOwner,
                &OnSoEInWorldLabel, nullptr)) {
            InWorldMode.store(InWorldBackend::SoEInterop, std::memory_order_release);
            QualifyGroundQuantityReader();
            const auto getStyle = reinterpret_cast<GetSoEStyleFn>(
                GetProcAddress(soe, SoE::Interop::InWorldLabelStyleExportName));
            const auto* style = getStyle ? getStyle() : nullptr;
            const bool styleReady = style &&
                style->structSize >= sizeof(SoE::Interop::InWorldLabelStyleApiV2) &&
                style->abiVersion == SoE::Interop::InWorldLabelStyleAbiV2 &&
                style->isReady && style->registerTransformer &&
                style->unregisterTransformer && style->isReady() &&
                style->registerTransformer(LootFilterInteropOwner,
                    &OnSoEInWorldStyle, nullptr);
            InWorldStyleAttached.store(styleReady, std::memory_order_release);
            const auto getScope = reinterpret_cast<GetSoERenderScopeFn>(
                GetProcAddress(soe, SoE::Interop::InWorldRenderScopeExportName));
            const auto* scope = getScope ? getScope() : nullptr;
            const bool scopeReady = scope &&
                scope->structSize >= sizeof(SoE::Interop::InWorldRenderScopeApiV3) &&
                scope->abiVersion == SoE::Interop::InWorldRenderScopeAbiV3 &&
                scope->isReady && scope->getCurrentItem && scope->isReady();
            InWorldRenderScopeApi.store(scopeReady ? scope : nullptr,
                std::memory_order_release);

            Emit(styleReady ?
                "LOOT_INWORLD_STYLE_READY version=1.0.0 mode=soe-interop-v2 labelText=name+native-palette only backgroundRGBA=unqualified nativeHooksAdded=0" :
                "LOOT_INWORLD_STYLE_UNAVAILABLE version=1.0.0 old-SoE-or-registration-refused hidden-hover-pass-through=1");
            Emit("LOOT_INWORLD_BACKEND version=1.0.0 mode=soe-interop-v1 owner=soe hook=0xC0420 extraHooksAdded=0 textObserve=1 style=optional-v2");
            return;
        }
        // A loaded SoE may own the hook even if its API is absent/not ready.
        // Fail CLOSED rather than try to hook through a foreign detour.
        InWorldMode.store(InWorldBackend::Blocked, std::memory_order_release);
        Emit("LOOT_INWORLD_BLOCKED soe-present-no-qualified-interop conflictingHookAttempt=0");
        return;
    }
    if (!OriginalGetItemCode || !Context->CheckExpectedBytes(
        InWorldFormatterRva, ExpectedInWorldFormatter.data(),
        static_cast<std::uint32_t>(ExpectedInWorldFormatter.size()))
        || !Context->InstallInlineHook(InWorldFormatterRva,
            ExpectedInWorldFormatter.data(),
            static_cast<std::uint32_t>(ExpectedInWorldFormatter.size()),
            &StandaloneInWorldIdentityHook, &OriginalInWorldFormatter)
        || !OriginalInWorldFormatter) {
        InWorldMode.store(InWorldBackend::Blocked, std::memory_order_release);
        Emit("LOOT_INWORLD_BLOCKED standalone-hook-unavailable no-blind-chain=1");
        return;
    }
    InWorldMode.store(InWorldBackend::StandaloneIdentity,
        std::memory_order_release);
    Emit("LOOT_INWORLD_BACKEND version=1.0.0 mode=standalone-identity-only owner=loot-filter hook=0xC0420 textObserve=0 writes=0");
}

// Startup already activates a valid JSON, but allow a configuration copied
// into place between plugin initialization and the first game join. We never
// parse the file from a native formatter/paint callback.
bool ReloadFilterRules();
bool ActivateConfiguredFilter(bool automatic) noexcept;
void RebaselineSoundAtGameJoin() noexcept;
void __cdecl OnInWorldGameJoined(const D2RL::PluginContext*,
    const D2RL::Lifecycle::GameplayEvent* event, void*) noexcept {
    if (!event || event->kind != D2RL::Lifecycle::GameplayEventKind::GameJoined)
        return;
    // Refresh the copied ground-item marker registry for the new game session.
    ResetMinimapTracking();
    if(AutomapProjectionHookInstalled.load(std::memory_order_acquire) &&
       std::string_view(MinimapOverlayRenderer::ActiveBackendName())=="none")
        (void)InitializeMinimapMarkerRenderer();
    TryAttachInWorldBackend();
    if (!std::atomic_load_explicit(&PublishedFilterRules,
            std::memory_order_acquire) && !FilterConfigPath.empty() &&
        ReloadFilterRules())
        (void)ActivateConfiguredFilter(true);
    ResetNativeRowLiveSession();
    EnableAutomaticNativeHover();

    RebaselineSoundAtGameJoin();
}

void RegisterInWorldLifecycle() noexcept {
    if (!Context) return;
    const D2RL::LifecycleServiceV1* service{};
    if (Context->QueryService(D2RL::ServiceId::Lifecycle,
            D2RL::LifecycleServiceV1Version,
            &service) != D2RL::ServiceQueryResult::Success
        || !D2RL::HasLifecycleServiceV1Field(service,
            D2RL::LifecycleServiceV1RequiredSize)
        || !service->registerGameplayEventListener) {
        Emit("LOOT_INWORLD_LIFECYCLE_UNAVAILABLE automatic-hidden-hover-requires-lifecycle");
        return;
    }
    const D2RL::Lifecycle::GameplayEventListener listener{
        D2RL::Lifecycle::GameplayEventListenerSize, 0,
        D2RL::Lifecycle::GameplayEventKind::GameJoined, 0,
        &OnInWorldGameJoined, nullptr
    };
    if (service->registerGameplayEventListener(Context, &listener,
            &InWorldJoinedListener) == D2RL::Lifecycle::Result::Success
        && InWorldJoinedListener != D2RL::Lifecycle::InvalidHandle) {
        InWorldLifecycle = service;
        Emit("LOOT_INWORLD_LIFECYCLE_READY backend-deferred-until-GameJoined");
    } else Emit("LOOT_INWORLD_LIFECYCLE_REFUSED automatic-hidden-hover-inactive");
}

void DetachInWorldInterop() noexcept {
    InWorldRenderScopeApi.store(nullptr, std::memory_order_release);
    if (InWorldMode.load(std::memory_order_acquire)
        != InWorldBackend::SoEInterop) return;
    if (auto* soe = GetModuleHandleW(L"d2rl-soe.dll")) {
        if (InWorldStyleAttached.exchange(false, std::memory_order_acq_rel)) {
            const auto getStyle = reinterpret_cast<GetSoEStyleFn>(
                GetProcAddress(soe, SoE::Interop::InWorldLabelStyleExportName));
            const auto* style = getStyle ? getStyle() : nullptr;
            if (style && style->structSize >= sizeof(SoE::Interop::InWorldLabelStyleApiV2)
                && style->abiVersion == SoE::Interop::InWorldLabelStyleAbiV2
                && style->unregisterTransformer)
                (void)style->unregisterTransformer(LootFilterInteropOwner,
                    &OnSoEInWorldStyle, nullptr);
        }
        const auto getApi = reinterpret_cast<GetSoEInteropFn>(
            GetProcAddress(soe, SoE::Interop::InWorldLabelExportName));
        const auto* api = getApi ? getApi() : nullptr;
        if (api && api->structSize >= sizeof(SoE::Interop::InWorldLabelApiV1)
            && api->abiVersion == SoE::Interop::InWorldLabelAbiV1
            && api->unregisterObserver)
            (void)api->unregisterObserver(LootFilterInteropOwner,
                &OnSoEInWorldLabel, nullptr);
    }
    InWorldMode.store(InWorldBackend::Blocked, std::memory_order_release);
}

void ReportInWorldStatus() noexcept {
    const auto mode = InWorldMode.load(std::memory_order_acquire);
    const char* name = mode == InWorldBackend::SoEInterop ? "soe-interop-v1" :
        mode == InWorldBackend::StandaloneIdentity ? "standalone-identity" :
        mode == InWorldBackend::Blocked ? "blocked" : "pending";
    InWorldSample last{};
    { std::lock_guard lock(InWorldSampleMutex); last = LastInWorldSample; }
    char prefixHex[3 * 16 + 1]{};
    for (std::size_t i = 0; i < std::min<std::size_t>(last.sourceLength, 16U); ++i)
        std::snprintf(prefixHex + 3 * i, sizeof(prefixHex) - 3 * i,
            "%02X ", last.firstBytes[i]);
    char printableCode[5]{};
    for (unsigned i = 0; i < 4; ++i) {
        const auto byte = static_cast<unsigned char>((last.code >> (8U * i)) & 0xFFU);
        printableCode[i] = byte == 0U ? ' ' : (byte >= 32U && byte <= 126U
            ? static_cast<char>(byte) : '?');
    }
    char message[480]{};
    std::snprintf(message, sizeof(message),
        "LOOT_INWORLD_STATUS version=1.0.0 mode=%s itemCalls=%llu divineCalls=%llu itemTextCalls=%llu type=%d classId=%u unitId=%u code='%s' rawCode=0x%08X sourceBytes=%u rawPrefixHex='%s' mutation=optional-soe-v2",
        name, static_cast<unsigned long long>(InWorldCalls.load()),
        static_cast<unsigned long long>(InWorldDivineCalls.load()),
        static_cast<unsigned long long>(InWorldTextCalls.load()),
        last.type, last.classId, last.unitId,
        printableCode, last.code, last.sourceLength, prefixHex);
    Emit(message);
    std::snprintf(message, sizeof(message),
        "LOOT_INWORLD_STYLE_STATUS version=1.0.0 attached=%u callbacks=%llu applied=%llu noRule=%llu unsupportedPalette=%llu guarded=%llu background=not-qualified standaloneText=identity-only",
        InWorldStyleAttached.load() ? 1U : 0U,
        static_cast<unsigned long long>(InWorldStyleCalls.load()),
        static_cast<unsigned long long>(InWorldStyleWrites.load()),
        static_cast<unsigned long long>(InWorldStyleNoRule.load()),
        static_cast<unsigned long long>(InWorldStyleUnsupportedColor.load()),
        static_cast<unsigned long long>(InWorldStyleGuarded.load()));
    Emit(message);
}
constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .apiVersion = D2RL_PLUGIN_API_VERSION,
    .id = "loot-filter",
    .name = "Loot Filter",
    .version = "1.0.0",
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
        std::strstr(message,"LOOT_INWORLD_BLOCKED") ||
        std::strstr(message,"LOOT_INWORLD_LIFECYCLE_UNAVAILABLE") ||
        std::strstr(message,"LOOT_INWORLD_LIFECYCLE_REFUSED") ||
        std::strstr(message,"LOOT_NATIVE_HOVER_UNAVAILABLE") ||
        std::strstr(message,"LOOT_GLYPH_B_REFUSED") ||
        std::strstr(message,"LOOT_PICKUP_GUARD_REFUSED") ||
        std::strstr(message,"LOOT_PICKUP_GUARD_INACTIVE") ||
        std::strstr(message,"LOOT_NATIVE_ACTION_REFUSED") ||
        std::strstr(message,"LOOT_NATIVE_ACTION_PARTIAL") ||
        std::strstr(message,"LOOT_NATIVE_ACTION_UNAVAILABLE") ||
        std::strstr(message,"LOOT_SOUND_REFUSED") ||
        std::strstr(message,"LOOT_SOUND_QUALIFY refused=") ||
        std::strstr(message,"LOOT_MINIMAP_RENDERER_REFUSED");
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
        std::strstr(message,"LOOT_NATIVE_HOVER_READY") ||
        std::strstr(message,"LOOT_SOUND_QUALIFY matched=") ||
        std::strstr(message,"LOOT_SOUND_ARMED") ||
        std::strstr(message,"LOOT_MINIMAP_PROJECTION_READY") ||
        std::strstr(message,"LOOT_MINIMAP_AUTOMAP_GATE_READY") ||
        std::strstr(message,"LOOT_MINIMAP_RENDERER_READY");
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

// 0.2.0: EXPLICIT-OPT-IN read-only native render-element correlation.
// Unlike the reverted 0x657B90 detour, this observer is on the higher-level
// D2R+0x8DA7E0 one-pointer native UI row renderer. It is still shared by
// multiple tooltips: *never* mutate its object or assign it to an item based
// solely on geometry or hover timing. The hook is absent until the user runs
// `loot-filter native-row-arm` and fails closed on unexpected bytes.
// It forwards the ONE RCX argument exactly once; 0x880BC7 sets RCX to
// [rowArray]+index*0x2E8, then ignores the callee return value. The callee's
// 93847 prologue and first consumers only take RCX as incoming argument.
constexpr std::uintptr_t NativeRowRendererRva=0x8DA7E0;
constexpr std::uintptr_t NativeRowCallerRva=0x880BC7;
constexpr std::uintptr_t NativeRowCallerReturnRva=0x880BCC;
constexpr std::uintptr_t NativeRowColorLeaRva=0x8DA91B;
// Native styled-text row queue: atlas build 92777 ABI, but EXACT bytes and
// direct callsite were independently observed in the user's 93847 captures.
// Shared with other UI consumers. Never chain an unqualified foreign hook.
constexpr std::uintptr_t NativeRowAppendRva=0x880160;
constexpr std::uintptr_t NativeRowAppendCallRva=0x843D87;
constexpr std::uintptr_t NativeRowAppendReturnRva=0x843D8C;
using NativeRowAppendFn=void(__fastcall*)(void*,const void*,const void*,
    const void*,const void*) noexcept;
NativeRowAppendFn OriginalNativeRowAppend{};
std::atomic_bool NativeRowAppendHookInstalled{};
constexpr std::size_t NativeRowAppendMaxEvents=256;
std::atomic<std::uint32_t> NativeRowAppendsTaken{};
// Completed append ordinal, never row-pointer identity. Published only AFTER
// the bounded event is written to its phase bucket. Renderer reads a fence.
std::atomic<std::uint64_t> NativeRowAppendSequence{};
std::atomic<std::uint64_t> NativeRowAppendCommittedSequence{};
using NativeRowRendererFn=void(__fastcall*)(void*) noexcept;
NativeRowRendererFn OriginalNativeRowRenderer{};
std::atomic_bool NativeRowRendererHookInstalled{};
std::atomic_flag NativeRowBgDrawGate=ATOMIC_FLAG_INIT;
thread_local bool NativeRowBgInsideRenderer=false;
// Automatic, persistent per-game JSON backgroundColor, enabled after GameJoined
// when SoE V1/V3 and the native append-to-renderer chain are qualified.
std::atomic_bool NativeRowBgLiveEnabled{};
std::atomic<std::uint64_t> NativeRowBgLiveEpoch{1};
std::atomic<std::uint64_t> NativeRowBgLiveLabels{},NativeRowBgLiveAppends{};
std::atomic<std::uint64_t> NativeRowBgLiveAttempts{},NativeRowBgLiveWrites{};
std::atomic<std::uint64_t> NativeRowBgLiveRestored{},NativeRowBgLiveRejected{};
std::atomic<std::uint64_t> NativeRowBgLiveNoRule{},NativeRowBgLiveRejectedColor{};
std::atomic<std::uint64_t> NativeRowBgLiveBusy{},NativeRowBgLiveRestoreAnomaly{};
std::atomic<std::uint32_t> NativeRowBgLiveLastUnitId{},NativeRowBgLiveLastCode{};
// Automatic native glyph color. NO new hooks or item writes. Only glyph-B
// nested inside an already-qualified, background-colored native row is eligible.
std::atomic_bool NativeRowFontColorEnabled{};
std::atomic<std::uint64_t> NativeRowFontColorAttempts{};
std::atomic<std::uint64_t> NativeRowFontColorForwarded{};
std::atomic<std::uint64_t> NativeRowFontColorRejectedCaller{};
std::atomic<std::uint64_t> NativeRowFontColorRejectedEpoch{};
std::atomic<std::uint64_t> NativeRowFontColorRejectedRule{};
std::atomic<std::uint64_t> NativeRowFontColorRejectedNative{};
std::atomic<std::uint64_t> NativeRowFontColorReadFailures{};
std::atomic<std::uint64_t> NativeRowFontColorLastAppendSeq{};
std::atomic<std::uint32_t> NativeRowFontColorLastUnitId{};
std::atomic<std::uint32_t> NativeRowFontColorLastCode{};
std::array<std::atomic<std::uint32_t>,4> NativeRowFontColorLastOriginalBits{};
std::array<std::atomic<std::uint32_t>,4> NativeRowFontColorLastForwardedBits{};

void ResetNativeRowLiveSession() noexcept {
    NativeRowFontColorEnabled.store(false,std::memory_order_release);
    NativeRowBgLiveEnabled.store(false,std::memory_order_release);
    NativeRowBgLiveEpoch.fetch_add(1,std::memory_order_acq_rel);
}
// Qualified same-thread font-color scope active only during the native row draw.
// Do not infer glyph ownership from a heartbeat or global shared renderer.
struct NativeRowFontDraw final {
    bool active{},hasTextRule{};
    std::uint32_t unitId{},code{};
    std::uint64_t appendSequence{},sessionEpoch{};
    std::array<std::uint32_t,4> configuredGlyphBits{};
};
thread_local NativeRowFontDraw NativeRowFontCurrentDraw{};
void BeginNativeRowFontDraw(std::uint32_t unitId,std::uint32_t code,
    std::uint64_t appendSeq,const GroundRuleDecision& rule) noexcept {
    if (!NativeRowFontColorEnabled.load(std::memory_order_acquire) ||
        NativeRowFontCurrentDraw.active) return;
    auto& draw=NativeRowFontCurrentDraw;
    draw={};draw.active=true;draw.unitId=unitId;draw.code=code;
    draw.appendSequence=appendSeq;
    draw.sessionEpoch=NativeRowBgLiveEpoch.load(std::memory_order_acquire);
    draw.hasTextRule=rule.hasTextColor;
    if (rule.hasTextColor)
        std::memcpy(draw.configuredGlyphBits.data(),rule.textColor.data(),
            sizeof(draw.configuredGlyphBits));
}
void EndNativeRowFontDraw() noexcept { NativeRowFontCurrentDraw={}; }
void ResetNativeRowFontColor() noexcept {
    NativeRowFontColorEnabled.store(false,std::memory_order_release);
}

// The game uses this render-object address as a reusable UI slot. A matching
// address across phases does not mean it belongs to the same inventory item.
// Native strings at +0x00/+0x28 are *candidate* text fields, based on the
// independently observed SoE tooltip-row layout (not item ownership proof).
struct NativeRowTextWitness final {
    std::uint64_t size{},capacity{},encodedCapacity{};
    // state: 0=implausible-header, 1=empty, 2=inline, 3=heap,
    // 4=heap-unreadable. Printable ASCII only; control/UTF-8 bytes -> '.'.
    std::uint8_t state{};
    std::uint8_t captured{};
    std::array<char,65> preview{};
    // First 24 raw bytes retained as hex so UTF-8 / native color escapes
    // remain distinguishable from printable ASCII placeholders.
    std::array<char,49> rawHex{};
    // Bounded copied bytes permit offline comparison with V1 native source.
    std::array<std::uint8_t,64> raw{};
};

constexpr std::size_t NativeRowLabelMaxBytes=64;
struct NativeRowLiveLabel final {
    std::uint64_t epoch{},sequence{},rulesGeneration{};
    std::uint32_t unitId{},classId{},code{},sourceLength{};
    std::uint32_t displayLength{};
    std::uint32_t quantity{};
    bool quantityKnown{};
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
    std::int64_t qpc{};
    bool scopeMatches{};
    // Original V1 source proves item/event association; the V2 replacement
    // is the exact string that the native row should actually contain.
    std::array<std::uint8_t,NativeRowLabelMaxBytes> source{},display{};
};
thread_local NativeRowLiveLabel NativeRowLiveLatestLabel{};
thread_local std::uint64_t NativeRowLiveNextLabelSeq{};
// Live fast path observes SoE V1 item callbacks and retains only immutable
// identity/property bytes needed to qualify the matching native row.
void ObserveNativeRowLiveLabel(std::int32_t type,
    std::uint32_t classId,std::uint32_t unitId,std::uint32_t rawCode,
    const void* nativeUnit,const char* source,std::uint32_t sourceLength) noexcept {
    if (!NativeRowBgLiveEnabled.load(std::memory_order_acquire)) return;
    NativeRowLiveLatestLabel={}; // invalid inputs invalidate old identity
    if (type!=4 || !unitId || !source || !sourceLength ||
        sourceLength>NativeRowLabelMaxBytes) return;
    const auto* scope=InWorldRenderScopeApi.load(std::memory_order_acquire);
    if (!scope || !scope->getCurrentItem) return;
    SoE::Interop::InWorldActiveItemV3 active{};
    active.structSize=sizeof(active);
    if (!scope->getCurrentItem(&active) || active.unitType!=4 ||
        active.unitId!=unitId || active.classId!=classId) return;
    const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (!rules || (!rules->backgroundRules && !rules->textColorRules &&
        !rules->hiddenRules)) return;
    LARGE_INTEGER stamp{};
    if (!QueryPerformanceCounter(&stamp)) return;
    NativeRowLiveLatestLabel.epoch=NativeRowBgLiveEpoch.load(
        std::memory_order_acquire);
    NativeRowLiveLatestLabel.sequence=++NativeRowLiveNextLabelSeq;
    NativeRowLiveLatestLabel.rulesGeneration=rules->generation;
    NativeRowLiveLatestLabel.unitId=unitId;
    NativeRowLiveLatestLabel.classId=classId;
    NativeRowLiveLatestLabel.code=CanonicalItemCode(rawCode);
    if(nativeUnit && (rules->usesQuality || rules->usesItemLevel ||
                      rules->usesSockets || rules->usesEthereal ||
                      rules->usesIdentified)) {
        const auto item=GroundRuleItem(rawCode,nativeUnit,rules.get(),unitId,
            GroundPropertyLive::Purpose::VerifiedLabel);
        NativeRowLiveLatestLabel.qualityKnown=item.qualityKnown;
        NativeRowLiveLatestLabel.quality=item.quality;
        NativeRowLiveLatestLabel.itemLevelKnown=item.itemLevelKnown;
        NativeRowLiveLatestLabel.itemLevel=item.itemLevel;
        NativeRowLiveLatestLabel.socketsKnown=item.socketsKnown;
        NativeRowLiveLatestLabel.sockets=item.sockets;
        NativeRowLiveLatestLabel.etherealKnown=item.etherealKnown;
        NativeRowLiveLatestLabel.ethereal=item.ethereal;
        NativeRowLiveLatestLabel.identifiedKnown=item.identifiedKnown;
        NativeRowLiveLatestLabel.identified=item.identified;
    }
    if(rules->usesQuantity && nativeUnit &&
       GroundQuantityReader.load(std::memory_order_acquire)) {
        const auto count=GroundStackQuantity(nativeUnit);
        NativeRowLiveLatestLabel.quantityKnown=true;
        NativeRowLiveLatestLabel.quantity=count>1?count:1;
    }
    NativeRowLiveLatestLabel.sourceLength=sourceLength;
    NativeRowLiveLatestLabel.displayLength=sourceLength;
    NativeRowLiveLatestLabel.qpc=stamp.QuadPart;
    NativeRowLiveLatestLabel.scopeMatches=true;
    std::memcpy(NativeRowLiveLatestLabel.source.data(),source,sourceLength);
    std::memcpy(NativeRowLiveLatestLabel.display.data(),source,sourceLength);
    NativeRowBgLiveLabels.fetch_add(1,std::memory_order_relaxed);
}

void ObserveNativeRowLiveReplacement(std::uint32_t classId,
    std::uint32_t unitId, std::uint32_t rawCode,
    std::string_view source, std::string_view rendered) noexcept {
    if (!NativeRowBgLiveEnabled.load(std::memory_order_acquire)) return;
    auto& event=NativeRowLiveLatestLabel;
    // V1 has already qualified the active SoE V3 item. Never grant a V2
    // callback for a different item, an old session or different JSON rules
    // access to this render row, even when item names happen to be identical.
    // atomic_load_explicit returns shared_ptr<const FilterRuleTable>, not
    // a raw pointer. Keep this owning snapshot alive across generation check.
    const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (!event.scopeMatches || !rules ||
        rules->generation!=event.rulesGeneration ||
        event.epoch!=NativeRowBgLiveEpoch.load(std::memory_order_acquire) ||
        !NativeRowLiveDisplayMatch::SameEvent(
            event.classId,event.unitId,event.code,
            std::string_view(reinterpret_cast<const char*>(event.source.data()),
                event.sourceLength),classId,unitId,rawCode,source)) return;
    // If transformed text is too long for the exact native-row witness,
    // disable coloring for this item rather than match an unrelated label.
    if (rendered.empty() || rendered.size()>event.display.size()) {
        event.scopeMatches=false;
        return;
    }
    event.display.fill(0);
    event.displayLength=static_cast<std::uint32_t>(rendered.size());
    std::memcpy(event.display.data(),rendered.data(),rendered.size());
}

struct NativeStyledTextVector final {
    std::uintptr_t data{};
    std::uint64_t count{},capacity{};
};
static_assert(sizeof(NativeStyledTextVector)==0x18);
struct NativeRowLiveAppend final {
    std::uint64_t epoch{},appendSequence{},labelSequence{},rulesGeneration{};
    std::uintptr_t component{},data{},row{};
    std::int64_t qpc{};
    std::uint32_t unitId{},classId{},code{},sourceLength{};
    std::uint32_t displayLength{};
    std::uint32_t quantity{};
    bool quantityKnown{};
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
    std::array<std::uint8_t,NativeRowLabelMaxBytes> source{},display{};
    bool qualified{};
};
thread_local NativeRowLiveAppend NativeRowLiveLastAppend{};
thread_local std::uint64_t NativeRowLiveNextAppendSeq{};
bool ReadNativeStyledVector(void*,NativeStyledTextVector&) noexcept;
void __fastcall HookNativeRowAppend(void*,const void*,const void*,
    const void*,const void*) noexcept;
// Bounded native-row text reader. Do not chase pointers until
// the 32-byte candidate has a plausible MSVC string length and capacity.
// We do not retain the pointer; the preview is copied synchronously.
bool ReadNativeRowTextBytes(std::uintptr_t address,
    void* out,std::size_t length) noexcept {
    if (!address || !out || !length || length>64) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void const*>(address),
        &region,sizeof(region)) || region.State!=MEM_COMMIT ||
        (region.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    const auto protection=region.Protect&0xFFU;
    const bool readable=protection==PAGE_READONLY ||
        protection==PAGE_READWRITE || protection==PAGE_WRITECOPY ||
        protection==PAGE_EXECUTE_READ ||
        protection==PAGE_EXECUTE_READWRITE ||
        protection==PAGE_EXECUTE_WRITECOPY;
    if (!readable) return false;
    const auto start=reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (address<start || length>region.RegionSize ||
        address-start>region.RegionSize-length) return false;
    std::memcpy(out,reinterpret_cast<void const*>(address),length);
    return true;
}

NativeRowTextWitness InspectNativeRowTextCandidate(
    const std::array<std::uint8_t,0x50>& headers,
    std::size_t offset,
    std::uintptr_t elementAddress) noexcept {
    NativeRowTextWitness result{};
    // 0.1.92's assumed MSVC size/capacity locations (+0x10/+0x18)
    // were disproven by the live 93847 headers. The candidate's layout is:
    // +0x00 pointer; +0x08 length; +0x10 capacity with high-bit
    // inline tag; +0x18 16-byte inline storage. For inline text, pointer
    // MUST equal the exact field address +0x18 (fail closed otherwise).
    const auto decoded=NativeRowStringLayout::Decode(
        headers,offset,elementAddress);
    result.size=decoded.size;
    result.capacity=decoded.capacity;
    result.encodedCapacity=decoded.encodedCapacity;
    if (!decoded.valid) return result;
    if (!result.size) {result.state=1;return result;}
    const auto take=static_cast<std::size_t>(
        std::min<std::uint64_t>(result.size,64));
    std::array<std::uint8_t,64> chars{};
    if (decoded.isInline) {
        std::memcpy(chars.data(),headers.data()+offset+0x18,take);
        result.state=2;
    } else {
        if (!ReadNativeRowTextBytes(decoded.pointer,chars.data(),take)) {
            result.state=4;return result;
        }
        result.state=3;
    }
    result.captured=static_cast<std::uint8_t>(take);
    result.raw=chars;
    constexpr char digits[]="0123456789ABCDEF";
    for(std::size_t i=0;i<take;++i) {
        result.preview[i]=chars[i]>=0x20 && chars[i]<=0x7E
            ? static_cast<char>(chars[i]):'.';
        if (i<24) {
            result.rawHex[2*i]=digits[chars[i]>>4];
            result.rawHex[2*i+1]=digits[chars[i]&0x0F];
        }
    }
    result.preview[take]='\0';
    return result;
}

// A single, bounded native heap read while the engine has passed us its row
// pointer synchronously. Refuse cross-region buffers and page guards.
// These are raw candidate fields, NOT yet a verified hovered-item identity.
bool SnapshotNativeRow(void* element,
    std::array<std::int32_t,4>& rect,
    std::array<std::uint32_t,4>& color,
    std::array<std::uint8_t,0x50>& textHeaders) noexcept {
    if (!element) return false;
    constexpr std::size_t bytes=0x178; // includes RGBA float4 at +0x168
    MEMORY_BASIC_INFORMATION region{};
    const auto address=reinterpret_cast<std::uintptr_t>(element);
    if (!VirtualQuery(element,&region,sizeof(region)) ||
        region.State!=MEM_COMMIT ||
        (region.Protect&(PAGE_GUARD|PAGE_NOACCESS))!=0 ||
        address<reinterpret_cast<std::uintptr_t>(region.BaseAddress) ||
        bytes>region.RegionSize ||
        address-reinterpret_cast<std::uintptr_t>(region.BaseAddress)>
            region.RegionSize-bytes) return false;
    std::array<std::uint8_t,bytes> snapshot{};
    std::memcpy(snapshot.data(),element,snapshot.size());
    std::memcpy(rect.data(),snapshot.data()+0x50,sizeof(rect));
    std::memcpy(color.data(),snapshot.data()+0x168,sizeof(color));
    std::memcpy(textHeaders.data(),snapshot.data(),textHeaders.size());
    return true;
}

bool TryNativeRowBgLive(void* element,std::uintptr_t caller) noexcept;

void __fastcall HookNativeRowRenderer(void* element) noexcept {
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (TryNativeRowBgLive(element,caller)) return;
    if (OriginalNativeRowRenderer) OriginalNativeRowRenderer(element);
}

void ArmNativeRowRuntime() noexcept {
    if (NativeRowRendererHookInstalled.load(std::memory_order_acquire) &&
        NativeRowAppendHookInstalled.load(std::memory_order_acquire)) {

        return;
    }
    const char* build=Context?D2RL::GetBuildName(Context):nullptr;
    if (!Context || !Base || !build || std::string_view(build)!="93847") {
        Emit("LOOT_NATIVE_ROW_REFUSED version=1.0.0 reason=build-or-base no-hook=1 no-fallback=1");
        return;
    }
    constexpr std::array<std::uint8_t,16> entry{{
        0x40,0x56,0x48,0x81,0xEC,0x10,0x01,0x00,
        0x00,0x48,0x8B,0x05,0xD8,0x0A,0x0F,0x02
    }};
    constexpr std::array<std::uint8_t,5> call{{0xE8,0x14,0x9C,0x05,0x00}};
    constexpr std::array<std::uint8_t,3> rcxToRsi{{0x48,0x8B,0xF1}};
    constexpr std::array<std::uint8_t,7> colorLea{{
        0x4C,0x8D,0xB6,0x68,0x01,0x00,0x00
    }};
    std::array<std::uint8_t,5> liveCall{};
    std::array<std::uint8_t,3> liveMov{};
    std::array<std::uint8_t,7> liveLea{};
    if (!ReadSafe(NativeRowCallerRva,liveCall.data(),liveCall.size()) ||
        liveCall!=call ||
        !ReadSafe(0x8DA802,liveMov.data(),liveMov.size()) ||
        liveMov!=rcxToRsi ||
        !ReadSafe(NativeRowColorLeaRva,liveLea.data(),liveLea.size()) ||
        liveLea!=colorLea ||
        !Context->CheckExpectedBytes(NativeRowRendererRva,entry.data(),
            static_cast<std::uint32_t>(entry.size()))) {
        Emit("LOOT_NATIVE_ROW_REFUSED version=1.0.0 reason=entry-caller-or-argument-fingerprint-mismatch potential-foreign-hook=1 no-fallback=1");
        return;
    }
    // The atlas's 92777 ABI is NOT enough alone: guard the entire 93847
    // append function entry, its sole direct caller, and vector offset.
    // If Extended Item Stats (or another plugin) owns this function entry,
    // refuse the experiment rather than chaining or overwriting its bridge.
    constexpr std::array<std::uint8_t,26> appendEntry{{
        0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,
        0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7C,0x24,0x20,
        0x41,0x56,0x48,0x83,0xEC,0x40
    }};
    constexpr std::array<std::uint8_t,5> appendCaller{{
        0xE8,0xD4,0xC3,0x03,0x00
    }};
    constexpr std::array<std::uint8_t,7> appendVector{{
        0x4C,0x8D,0xB1,0x68,0x01,0x00,0x00
    }};
    std::array<std::uint8_t,5> actualCaller{};
    std::array<std::uint8_t,7> actualVector{};
    if (!ReadSafe(NativeRowAppendCallRva,actualCaller.data(),actualCaller.size()) ||
        actualCaller!=appendCaller ||
        !ReadSafe(0x88017A,actualVector.data(),actualVector.size()) ||
        actualVector!=appendVector ||
        !Context->CheckExpectedBytes(NativeRowAppendRva,appendEntry.data(),
            static_cast<std::uint32_t>(appendEntry.size()))) {
        Emit("LOOT_NATIVE_ROW_APPEND_REFUSED version=1.0.0 reason=entry-caller-vector-fingerprint-or-foreign-owner nativeRowPhase=refused no-fallback=1 backgroundWrites=0");
        return;
    }
    if (!Context->InstallInlineHook(NativeRowAppendRva,appendEntry.data(),
        static_cast<std::uint32_t>(appendEntry.size()),
        HookNativeRowAppend,&OriginalNativeRowAppend) ||
        !OriginalNativeRowAppend) {
        Emit("LOOT_NATIVE_ROW_APPEND_REFUSED version=1.0.0 reason=loader-hook-install-failed no-fallback=1 backgroundWrites=0");
        return;
    }
    NativeRowAppendHookInstalled.store(true,std::memory_order_release);

    if (!Context->InstallInlineHook(NativeRowRendererRva,entry.data(),
        static_cast<std::uint32_t>(entry.size()),
        HookNativeRowRenderer,&OriginalNativeRowRenderer) ||
        !OriginalNativeRowRenderer) {
        Emit("LOOT_NATIVE_ROW_REFUSED version=1.0.0 reason=loader-hook-install-failed no-fallback=1");
        return;
    }
    NativeRowRendererHookInstalled.store(true,std::memory_order_release);

}



// Qualify a small heap memory range before copying. Never access stale or
// unrelated component pointers after this synchronous function invocation.
bool ReadNativeStyledVector(void* component,
    NativeStyledTextVector& vector) noexcept {
    if (!component) return false;
    const auto address=reinterpret_cast<std::uintptr_t>(component);
    constexpr std::uintptr_t offset=0x168;
    if (address>std::numeric_limits<std::uintptr_t>::max()-offset)
        return false;
    return ReadNativeRowTextBytes(address+offset,&vector,sizeof(vector));
}

// Production native-row write guard. Read the current row and latest completed
// append on this invocation; refuse styling when scope, sequence, vector,
// original text, or writable-range validation disagrees.
bool NativeRowBgWritable(void* element) noexcept {
    if (!element) return false;
    const auto address=reinterpret_cast<std::uintptr_t>(element);
    if (address>std::numeric_limits<std::uintptr_t>::max()-0x178U)
        return false;
    const auto color=address+0x168U;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void*>(color),&region,sizeof(region)) ||
        region.State!=MEM_COMMIT ||
        (region.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    const DWORD protection=region.Protect&0xFFU;
    if (protection!=PAGE_READWRITE &&
        protection!=PAGE_EXECUTE_READWRITE) return false;
    const auto base=reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    return base<=color && color-base<=region.RegionSize &&
        region.RegionSize-(color-base)>=sizeof(std::array<float,4>);
}

bool TryNativeRowBgLive(void* element,std::uintptr_t caller) noexcept {
    if (!NativeRowBgLiveEnabled.load(std::memory_order_acquire)) return false;
    if (caller!=Base+NativeRowCallerReturnRva || !element ||
        !OriginalNativeRowRenderer ||
        !NativeRowAppendHookInstalled.load(std::memory_order_acquire) ||
        !NativeRowRendererHookInstalled.load(std::memory_order_acquire))
        return false;
    NativeRowBgLiveAttempts.fetch_add(1,std::memory_order_relaxed);
    const auto append=NativeRowLiveLastAppend; // TLS: same render thread
    const auto epoch=NativeRowBgLiveEpoch.load(std::memory_order_acquire);
    LARGE_INTEGER now{},frequency{};
    if (!append.qualified || !append.epoch || append.epoch!=epoch ||
        !QueryPerformanceCounter(&now) ||
        !QueryPerformanceFrequency(&frequency) ||
        !NativeRowBgLivePolicy::RecentChain(
            append.qpc,now.QuadPart,frequency.QuadPart) ||
        !append.labelSequence || !append.unitId ||
        append.row!=reinterpret_cast<std::uintptr_t>(element) ||
        !NativeRowBgWritable(element)) {
        NativeRowBgLiveRejected.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    const auto current=NativeRowLiveLatestLabel;
    if (!NativeRowBgLivePolicy::SameLabel(
            append.epoch,epoch,append.labelSequence,current.sequence,
            append.unitId,current.unitId,append.classId,current.classId,
            append.code,current.code) ||
        current.rulesGeneration!=append.rulesGeneration ||
        !current.scopeMatches ||
        current.sourceLength!=append.sourceLength ||
        current.source!=append.source ||
        current.displayLength!=append.displayLength ||
        current.display!=append.display) {
        NativeRowBgLiveRejected.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (!rules || (!rules->backgroundRules && !rules->hiddenRules) ||
        rules->generation!=append.rulesGeneration) {
        NativeRowBgLiveNoRule.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    RuleEngine::Item rowItem{};
    rowItem.code=CanonicalItemCode(append.code);
    rowItem.classIdKnown=true;rowItem.classId=append.classId;
    rowItem.quantityKnown=append.quantityKnown;
    rowItem.quantity=append.quantity;
    rowItem.qualityKnown=append.qualityKnown;
    rowItem.quality=append.quality;
    rowItem.itemLevelKnown=append.itemLevelKnown;
    rowItem.itemLevel=append.itemLevel;
    rowItem.socketsKnown=append.socketsKnown;
    rowItem.sockets=append.sockets;
    rowItem.etherealKnown=append.etherealKnown;
    rowItem.ethereal=append.ethereal;
    rowItem.identifiedKnown=append.identifiedKnown;
    rowItem.identified=append.identified;
    GroundRuleDecision resolvedRule{};
    const auto* rule=ResolveGroundRule(rules.get(),rowItem,resolvedRule) ?
        &resolvedRule : nullptr;
    if (!rule) {
        NativeRowBgLiveNoRule.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    NativeStyledTextVector vector{};
    if (!ReadNativeStyledVector(
            reinterpret_cast<void*>(append.component),vector) ||
        vector.data!=append.data || vector.count!=1 ||
        vector.data!=append.row) {
        NativeRowBgLiveRejected.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    std::array<std::int32_t,4> rect{};
    std::array<std::uint32_t,4> nativeColor{};
    std::array<std::uint8_t,0x50> header{};
    if (!SnapshotNativeRow(element,rect,nativeColor,header)) {
        NativeRowBgLiveRejected.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    const auto text=InspectNativeRowTextCandidate(header,0,
        reinterpret_cast<std::uintptr_t>(element));
    const bool exact=text.size==append.displayLength &&
        text.captured==text.size && text.size!=0 &&
        std::memcmp(text.raw.data(),append.display.data(),
            append.displayLength)==0;
    if (!exact || !NativeRowBgPolicy::VanillaHiddenBlack(nativeColor)) {
        NativeRowBgLiveRejectedColor.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    // A hidden-hover label is a DIFFERENT draw path from bulk ground paint.
    // Only suppress a show:false row AFTER verifying the exact live SoE item,
    // generation, same-thread append, renderer row/vector, text, and native
    // background. Do not inspect Alt/input state and do not mutate native
    // objects or the item's pickup/interaction state. Unlike bulk ground
    // paint, this row renderer handles only visual output for this label.
    if (!rule->show) {
        LARGE_INTEGER finalNow{};
        const auto latest=NativeRowLiveLatestLabel;
        if (!NativeRowBgLiveEnabled.load(std::memory_order_acquire) ||
            NativeRowBgLiveEpoch.load(std::memory_order_acquire)!=epoch ||
            NativeRowLiveLastAppend.appendSequence!=append.appendSequence ||
            NativeRowLiveLastAppend.epoch!=epoch ||
            latest.sequence!=append.labelSequence ||
            latest.rulesGeneration!=append.rulesGeneration ||
            !QueryPerformanceCounter(&finalNow) ||
            !NativeRowBgLivePolicy::RecentChain(
                append.qpc,finalNow.QuadPart,frequency.QuadPart) ||
            rules->generation!=append.rulesGeneration ||
            !GroundVisibility::SuppressHiddenHover(
                HideGroundArmed.load(std::memory_order_acquire),
                current.scopeMatches && current.unitId==append.unitId &&
                    current.classId==append.classId && current.code==append.code,
                append.qualified && append.appendSequence!=0,
                append.row==reinterpret_cast<std::uintptr_t>(element),
                exact,latest.rulesGeneration==append.rulesGeneration,
                !rule->show)) {
            NativeRowBgLiveRejected.fetch_add(1,std::memory_order_relaxed);
            return false;
        }
        HiddenHoverRowsSuppressed.fetch_add(1,std::memory_order_relaxed);
        return true; // ONLY this fully verified hidden-hover row is not drawn
    }
    if (!rule->hasBackground) return false;
    if (NativeRowBgInsideRenderer ||
        NativeRowBgDrawGate.test_and_set(std::memory_order_acquire)) {
        NativeRowBgLiveBusy.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    NativeRowBgInsideRenderer=true;
    auto* color=reinterpret_cast<std::uint8_t*>(element)+0x168;
    std::array<std::uint32_t,4> original{},replacement{};
    std::memcpy(original.data(),color,sizeof(original));
    std::memcpy(replacement.data(),rule->background.data(),sizeof(replacement));
    LARGE_INTEGER finalNow{};
    const bool stillQualified=NativeRowBgLiveEnabled.load(
            std::memory_order_acquire) &&
        NativeRowBgLiveEpoch.load(std::memory_order_acquire)==epoch &&
        NativeRowLiveLastAppend.appendSequence==append.appendSequence &&
        NativeRowLiveLastAppend.epoch==epoch &&
        NativeRowLiveLatestLabel.sequence==append.labelSequence &&
        QueryPerformanceCounter(&finalNow) &&
        NativeRowBgLivePolicy::RecentChain(append.qpc,finalNow.QuadPart,
            frequency.QuadPart) &&
        original==nativeColor && replacement!=original &&
        rules->generation==append.rulesGeneration;
    if (!stillQualified) {
        NativeRowBgInsideRenderer=false;
        NativeRowBgDrawGate.clear(std::memory_order_release);
        NativeRowBgLiveRejected.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    // Exactly one temporary 16-byte native float4 modification, synchronous
    // native draw, restore on this same stack. No glyph/UI-global detours.
    std::memcpy(color,replacement.data(),sizeof(replacement));
    NativeRowBgLiveWrites.fetch_add(1,std::memory_order_relaxed);
    BeginNativeRowFontDraw(append.unitId,append.code,
        append.appendSequence,*rule);
    NativeRowBgLiveLastUnitId.store(append.unitId,
        std::memory_order_release);
    NativeRowBgLiveLastCode.store(append.code,
        std::memory_order_release);
    OriginalNativeRowRenderer(element); // exactly once on success
    std::array<std::uint32_t,4> after{};
    std::memcpy(after.data(),color,sizeof(after));
    if (after==replacement) {
        std::memcpy(color,original.data(),sizeof(original));
        NativeRowBgLiveRestored.fetch_add(1,std::memory_order_relaxed);
    } else {
        NativeRowBgLiveRestoreAnomaly.fetch_add(1,
            std::memory_order_relaxed);
        ResetNativeRowFontColor();
        NativeRowBgLiveEnabled.store(false,std::memory_order_release);
        NativeRowBgLiveEpoch.fetch_add(1,std::memory_order_acq_rel);
        // Native renderer changed color: never overwrite its newer value.
    }
    EndNativeRowFontDraw();
    NativeRowBgInsideRenderer=false;
    NativeRowBgDrawGate.clear(std::memory_order_release);
    return true;
}

// Qualified native append handoff. The actual ABI has 5 pointer
// arguments in RCX/RDX/R8/R9/stack (atlas, backed by 93847 native prologue).
// Forward all five to the loader trampoline EXACTLY ONCE; never hold a lock
// across the original call, and never log/allocate inside this hook.
void __fastcall HookNativeRowAppend(void* component,const void* text,
    const void* requestedRect,const void* style,
    const void* secondaryText) noexcept {
    const bool live=NativeRowBgLiveEnabled.load(std::memory_order_acquire);
    NativeRowLiveAppend candidate{};
    NativeStyledTextVector before{};
    if (live) {
        NativeRowLiveLastAppend={}; // any new append revokes previous identity
        candidate.epoch=NativeRowBgLiveEpoch.load(std::memory_order_acquire);
        candidate.appendSequence=++NativeRowLiveNextAppendSeq;
        const auto label=NativeRowLiveLatestLabel;
        candidate.labelSequence=label.sequence;
        candidate.rulesGeneration=label.rulesGeneration;
        candidate.unitId=label.unitId;
        candidate.classId=label.classId;
        candidate.code=label.code;
        candidate.quantity=label.quantity;
        candidate.quantityKnown=label.quantityKnown;
        candidate.qualityKnown=label.qualityKnown;
        candidate.quality=label.quality;
        candidate.itemLevelKnown=label.itemLevelKnown;
        candidate.itemLevel=label.itemLevel;
        candidate.socketsKnown=label.socketsKnown;
        candidate.sockets=label.sockets;
        candidate.etherealKnown=label.etherealKnown;
        candidate.ethereal=label.ethereal;
        candidate.identifiedKnown=label.identifiedKnown;
        candidate.identified=label.identified;
        candidate.sourceLength=label.sourceLength;
        candidate.source=label.source;
        candidate.displayLength=label.displayLength;
        candidate.display=label.display;
        candidate.component=reinterpret_cast<std::uintptr_t>(component);
        if (reinterpret_cast<std::uintptr_t>(_ReturnAddress())!=
            Base+NativeRowAppendReturnRva || !label.scopeMatches ||
            !label.epoch || label.epoch!=candidate.epoch) candidate={};
        else {
            const auto* scope=InWorldRenderScopeApi.load(std::memory_order_acquire);
            SoE::Interop::InWorldActiveItemV3 active{};
            active.structSize=sizeof(active);
            if (!scope || !scope->getCurrentItem ||
                !scope->getCurrentItem(&active) || active.unitType!=4 ||
                active.unitId!=label.unitId || active.classId!=label.classId)
                candidate={};
            else {
                LARGE_INTEGER stamp{},frequency{};
                if (!QueryPerformanceCounter(&stamp) ||
                    !QueryPerformanceFrequency(&frequency) ||
                    !NativeRowBgLivePolicy::RecentChain(
                        label.qpc,stamp.QuadPart,frequency.QuadPart) ||
                    !ReadNativeStyledVector(component,before) || before.count!=0)
                    candidate={};
            }
        }
    }
    if (OriginalNativeRowAppend)
        OriginalNativeRowAppend(component,text,requestedRect,style,secondaryText);
    if (!live || !candidate.appendSequence ||
        !NativeRowBgLiveEnabled.load(std::memory_order_acquire) ||
        candidate.epoch!=NativeRowBgLiveEpoch.load(std::memory_order_acquire) ||
        NativeRowLiveLatestLabel.sequence!=candidate.labelSequence ||
        NativeRowLiveLatestLabel.displayLength!=candidate.displayLength ||
        NativeRowLiveLatestLabel.display!=candidate.display) return;
    NativeStyledTextVector after{};
    if (!ReadNativeStyledVector(component,after) ||
        !NativeRowAppendMatch::ValidNewRow(after.data,before.count,
            after.count,after.capacity) ||
        !NativeRowBgPolicy::SingleRow(before.count,after.count)) return;
    candidate.data=after.data;
    candidate.row=after.data;
    std::array<std::int32_t,4> rect{};
    std::array<std::uint32_t,4> color{};
    std::array<std::uint8_t,0x50> header{};
    if (!SnapshotNativeRow(reinterpret_cast<void*>(candidate.row),
            rect,color,header) ||
        !NativeRowBgPolicy::VanillaHiddenBlack(color)) return;
    const auto rowText=InspectNativeRowTextCandidate(header,0,candidate.row);
    if (rowText.size!=candidate.displayLength ||
        rowText.captured!=rowText.size || rowText.size==0 ||
        std::memcmp(rowText.raw.data(),candidate.display.data(),
            candidate.displayLength)!=0) return;
    LARGE_INTEGER stamp{};
    if (!QueryPerformanceCounter(&stamp)) return;
    candidate.qpc=stamp.QuadPart;
    candidate.qualified=true;
    NativeRowLiveLastAppend=candidate;
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
// fallbacks for previous production/probe filenames when the canonical file is absent.
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
    const auto legacyProbe=directory/L"loot-filter-probe.json";
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
    if(std::filesystem::exists(legacyProbe,error) && !error) {
        FilterConfigPath=legacyProbe;
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

// Sample only from the background runtime worker. No file metadata or parser
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
            Emit("LOOT_RULES_REFUSED config-missing-empty-or-over-64KiB last-valid-rules-preserved=1");
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
            Emit("LOOT_RULES_REFUSED over-256-rules last-valid-rules-preserved=1");
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
                    "LOOT_RULES_REFUSED entry=%zu invalid-show-hide-block fields={conditions?,continue?,name?,tooltip?,dropSound?,minimapIcon?} last-valid-rules-preserved=1" :
                    "LOOT_RULES_REFUSED entry=%zu requires-code/conditions-and-action(show|name|tooltip|dropSound|minimapIcon) last-valid-rules-preserved=1",line);
                Emit(message);return false;
            }

            // v3 is intentionally strict: colors belong only inside tooltip.
            // v1/v2 keep the pre-0.2.71 flat aliases for migration.
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
                    if(key.key()!="conditions" && key.key()!="continue" &&
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
            "LOOT_RULES_LOADED version=1.0.0 schema=%u generation=%llu rules=%zu backgroundRules=%zu textColorRules=%zu soundRules=%zu minimapIconRules=%zu hiddenRules=%zu syntax=schema3:{show|hide:{conditions?,continue?,name?,tooltip?,dropSound?,minimapIcon?}} tooltip{backgroundColor,textColor}+RGBA(r,g,b,a) minimapShapes=circle|diamond|triangle|star minimapSizePx=default12,clamped12..40 maxNameBytes=%zu propertyQuality=%u propertyIlvl=%u propertySockets=%u propertyEthereal=%u propertyIdentified=%u propertyItemType=%u reload=atomic nativeItemWrites=0",
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
    Emit("LOOT_GEOMETRY_HOOK_READY version=1.0.0 hook=D2R+0xCBEB0 onlyCallerReturnRva=0x1FAA1D innerSize=0x80 textOffset=+0x04 geometryRecalculatedByNativeFormatter=1 mode=off");
}

// Record identity at the only point with a verified native item pointer.
// The painter may read a separate label record, so match unit ID, recent
// formatter observation, and exact label text; NEVER compare record pointers.
void RememberGroundIdentity(void* unit, void* record, bool sourceCall,
                           bool paired, std::uint8_t result) noexcept {
    if (ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules ||
        !sourceCall || !paired || !result || !unit || !record ||
        !OriginalGetItemCode || !HookInstalled.load(std::memory_order_acquire)) return;
    BackgroundQualified.fetch_add(1,std::memory_order_relaxed);
    std::uint32_t header[3]{},recordId{};
    SIZE_T copied{};
    const auto address=reinterpret_cast<std::uintptr_t>(record);
    if (address>UINTPTR_MAX-0x28-80 ||
        !ReadProcessMemory(GetCurrentProcess(),unit,header,sizeof(header),&copied) ||
        copied!=sizeof(header) || header[0]!=4 || header[2]==0) {
        BackgroundGuardFailures.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    copied=0;
    if (!ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address+0x10),&recordId,
        sizeof(recordId),&copied) || copied!=sizeof(recordId) ||
        recordId!=header[2]) {
        BackgroundGuardFailures.fetch_add(1,std::memory_order_relaxed);
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
    if(selected && (selected->hasBackground || selected->hasTextColor))
    if (!selected || (!selected->hasBackground &&
        !selected->hasTextColor && selected->show)) {
        BackgroundNoMatch.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    std::array<char,80> name{};
    copied=0;
    if (!ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address+0x28),name.data(),
        name.size(),&copied) || copied!=name.size() ||
        !std::memchr(name.data(),'\0',name.size())) {
        BackgroundGuardFailures.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    const auto visibleLength=strnlen_s(name.data(),name.size());
    const auto quantity=GroundStackQuantity(unit); // exact formatter unit
    if (selected->hasName &&
        !GroundQuantity::MatchesRuleName(
            std::string_view(name.data(),visibleLength),
            std::string_view(selected->name.data(),selected->bytes-1U),
            quantity)) {
        BackgroundGuardFailures.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    if (!GroundIdentityMutex.try_lock()) {
        GroundIdentitySkips.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    const auto now=GetTickCount64();
    const auto base=static_cast<std::size_t>(recordId)%IdentitySlots;
    std::size_t slot=IdentitySlots;
    for (std::size_t offset=0;offset<IdentityProbeSlots;offset++) {
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
        GroundIdentityUpdates.fetch_add(1,std::memory_order_relaxed);
        BackgroundMatched.fetch_add(1,std::memory_order_relaxed);
    } else GroundIdentitySkips.fetch_add(1,std::memory_order_relaxed);
    GroundIdentityMutex.unlock();
}

bool GetGroundIdentity(std::uint32_t id, std::string_view visibleName,
                       std::uint32_t& code,
                       std::uint32_t* quantityOut,
                       std::uint32_t* classIdOut,
                       RuleEngine::Item* scalarOut) noexcept {
    if (!id || !GroundIdentityMutex.try_lock()) {
        GroundIdentitySkips.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    const auto now=GetTickCount64();
    const auto base=static_cast<std::size_t>(id)%IdentitySlots;
    bool found=false;
    for (std::size_t offset=0;offset<IdentityProbeSlots;offset++) {
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

// 0.1.52 paint-time per-rule BACKGROUND and TEXT RGBA argument forwarding.
// No native label-color or item records are changed by this JSON path. The native pointer is restored
// automatically when the helper returns (no owned record state to restore).
// Set only while the original shared label painter is synchronously drawing
// a *verified* Divine ground label. Never match inventory/tooltips by text.
// Valid only during the synchronous verified ground-label paint call.
// The underlying float[4] is a stack-local copy, never a pointer into a
// published ruleset (and never retained by the renderer).
thread_local const float* VerifiedRuleGlyphColor=nullptr;
std::atomic_bool CorrectedGlyphBArmed{};

void __fastcall HookSharedLabelPaint(void* rect,void* textArg,void* colorArg) noexcept {
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const bool ground=caller==Base+0x1517AF6;
    const bool neighbor=caller==Base+0x1519E46;
    const bool observe=BackgroundPaintObserveArmed.load(std::memory_order_acquire);
    const bool tint=BackgroundTintArmed.load(std::memory_order_acquire) &&
        ActiveGeometryMode.load(std::memory_order_acquire)==GeometryMode::Rules;
    const bool hide=HideGroundArmed.load(std::memory_order_acquire) &&
        ActiveGeometryMode.load(std::memory_order_acquire)==GeometryMode::Rules;
    const bool glyphObserve=GroundTextObserveArmed.load(std::memory_order_acquire);
    const bool cyan=GroundTextCyanArmed.load(std::memory_order_acquire) &&
        ActiveGeometryMode.load(std::memory_order_acquire)==GeometryMode::Rules;
    // Only a verified configured ground-label rule may scope a glyph-B
    // override. Legacy record-color writer remains disabled.
    std::array<float,4> scopedRuleTextColor{};
    bool hasScopedRuleTextColor=false;
    bool glyphWasCyanOrPatched=false;
    void* glyphTarget=nullptr;
    void* forwardedColor=colorArg;
    std::array<float,4> forwardedRgba{}; // kept alive until original returns
    // Must outlive the qualification block: the native painter and glyph
    // color selection below read this flag after that block ends.
    bool concealGroundVisuals=false;
    // The native record identity is borrowed and scoped to the qualification
    // block below. Preserve only value copies for the post-block audit.
    std::uint32_t concealedRecordId{};
    std::uint32_t concealedVerifiedCode{};
    // 0.2.48: values copied from the qualified record while its locals are
    // in scope. Report BULK_PAINT only after the original renderer returns.
    bool latencyBulkBackgroundForwarded=false;
    std::uint32_t latencyBulkCode{};
    std::uint32_t latencyBulkRecordId{};
    if ((observe || tint || glyphObserve || cyan || hide) && (ground || neighbor)) {
        if (observe) {
            BackgroundPaintCalls.fetch_add(1,std::memory_order_relaxed);
            if (ground) BackgroundPaintGroundCalls.fetch_add(1,std::memory_order_relaxed);
            else BackgroundPaintNeighborCalls.fetch_add(1,std::memory_order_relaxed);
        }
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
        if (observe && (!colorOk || !idOk || !textOk))
            BackgroundPaintReadFailures.fetch_add(1,std::memory_order_relaxed);
        std::uint32_t verifiedCode{},verifiedQuantity{},verifiedClassId{};
        RuleEngine::Item verifiedProperties{};
        const auto nameLength=textOk?strnlen_s(textBytes.data(),textBytes.size()):0;
        const bool identified=idOk && textOk &&
            GetGroundIdentity(recordId,std::string_view(textBytes.data(),nameLength),
                verifiedCode,&verifiedQuantity,&verifiedClassId,
                &verifiedProperties);
        const bool divine=identified && verifiedCode==DivineCode;
        const bool map=identified && verifiedCode==PackFilterCode("mp04");
        // The 0x1517AF6 ground paint route is the game's bulk ground-label
        // path. It is independent of whether item labels are held or TOGGLED
        // on. Hidden-hover uses SoE's separately qualified native row path.
        //
        // Do NOT skip OriginalSharedLabelPaint: besides visual output its
        // native work may be required for hit testing / hover selection.
        // 0.2.10/0.2.11 skipped this call and a hidden ground item could no
        // longer be inspected with labels hidden. Suppress only its visual
        // background + glyph alpha, forwarding the native painter once.
        if (hide && ground && paired && colorOk && identified &&
            CorrectedGlyphBArmed.load(std::memory_order_acquire)) {
            const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
                std::memory_order_acquire);
            const auto ruleItem=CachedGroundRuleItem(verifiedCode,verifiedClassId,
                verifiedQuantity,rules.get(),&verifiedProperties);
            GroundRuleDecision resolvedRule{};
            const auto* rule=ResolveGroundRule(rules.get(),ruleItem,resolvedRule) ?
                &resolvedRule : nullptr;
            if (rule) {
                const bool nameMatches=!rule->hasName ||
                    GroundQuantity::MatchesRuleName(
                        std::string_view(textBytes.data(),nameLength),
                        std::string_view(rule->name.data(),rule->bytes-1U),
                        verifiedQuantity);
                concealGroundVisuals=GroundVisibility::ConcealBulkVisuals(
                    true,ground,paired,colorOk,identified,
                    !rule->show,nameMatches);
            }
            if (concealGroundVisuals) {
                concealedRecordId=recordId;
                concealedVerifiedCode=verifiedCode;
            }
        }
        if (ground && paired && colorOk && identified &&
            ActiveGeometryMode.load(std::memory_order_relaxed)==GeometryMode::Rules &&
            CorrectedGlyphBArmed.load(std::memory_order_relaxed)) {
            const auto snapshot=std::atomic_load_explicit(&PublishedFilterRules,
                std::memory_order_acquire);
            const auto ruleItem=CachedGroundRuleItem(verifiedCode,verifiedClassId,
                verifiedQuantity,snapshot.get(),&verifiedProperties);
            GroundRuleDecision resolvedRule{};
            const auto* candidate=ResolveGroundRule(snapshot.get(),ruleItem,resolvedRule) ?
                &resolvedRule : nullptr;
            if (candidate && candidate->hasTextColor &&
                (!candidate->hasName || GroundQuantity::MatchesRuleName(
                    std::string_view(textBytes.data(),nameLength),
                    std::string_view(candidate->name.data(),candidate->bytes-1U),
                    verifiedQuantity))) {
                scopedRuleTextColor=candidate->textColor;
                hasScopedRuleTextColor=true;
            }
        }
        bool customForwarded=false;
        // The 0.1.32 samples established +0xAC/+0xB0/+0xB4/+0xB8
        // as four DWORD channel values. Observe before any opt-in edit.
        // Only inspect the normal 0x144-byte ground record + verified ID.
        if ((glyphObserve || cyan) && ground) {
            if (!paired || !idOk || !textOk) {
                GroundTextReadFailures.fetch_add(1,std::memory_order_relaxed);
            } else if (!identified) {
                GroundTextUnverified.fetch_add(1,std::memory_order_relaxed);
            } else {
                std::array<std::uint32_t,4> nativeGlyph{};
                const auto glyphAddress=recordAddress+GroundGlyphColorOffset;
                SIZE_T glyphCopied{};
                const bool glyphRead=ReadProcessMemory(GetCurrentProcess(),
                    reinterpret_cast<const void*>(glyphAddress),nativeGlyph.data(),
                    sizeof(nativeGlyph),&glyphCopied) && glyphCopied==sizeof(nativeGlyph);
                if (!glyphRead) GroundTextReadFailures.fetch_add(1,std::memory_order_relaxed);
                else {
                    GroundTextSamples.fetch_add(1,std::memory_order_relaxed);
                    if (divine) GroundTextDivine.fetch_add(1,std::memory_order_relaxed);
                    if (map) GroundTextMap.fetch_add(1,std::memory_order_relaxed);
                    if (glyphObserve && (divine || map) &&
                        GroundGlyphColorSamplesMutex.try_lock()) {
                        auto& dest=divine?GroundDivineGlyphSample:GroundMapGlyphSample;
                        dest.rgba=nativeGlyph;
                        dest.unitId=recordId;
                        ++dest.hits;
                        dest.valid=true;
                        GroundGlyphColorSamplesMutex.unlock();
                    }
                    if (divine && (cyan || nativeGlyph==ProbeCyanGlyphColor)) {
                        // Do not alter native map text or any non-verified ID.
                        // Only write the exact native Divine color or the qualified
                        // OWN distinctive cyan; refuse unknown style values.
                        if (cyan && nativeGlyph==ProbeCyanGlyphColor) {
                            glyphWasCyanOrPatched=true;
                            glyphTarget=reinterpret_cast<void*>(glyphAddress);
                            GroundTextCyanAlreadyPresent.fetch_add(1,std::memory_order_relaxed);
                        } else if (nativeGlyph==NativeDivineGlyphColor ||
                                   (!cyan && nativeGlyph==ProbeCyanGlyphColor)) {
                            if (!GroundTextCyanWriteMutex.try_lock())
                                GroundTextCyanContended.fetch_add(1,std::memory_order_relaxed);
                            else {
                                glyphTarget=reinterpret_cast<void*>(glyphAddress);
                                if (WritableRange(glyphTarget,sizeof(nativeGlyph))) {
                                    const auto& desired=cyan?ProbeCyanGlyphColor:NativeDivineGlyphColor;
                                    std::memcpy(glyphTarget,desired.data(),sizeof(nativeGlyph));
                                    if (cyan) {
                                        glyphWasCyanOrPatched=true;
                                        GroundTextCyanWrites.fetch_add(1,std::memory_order_relaxed);
                                    } else {
                                        GroundTextCyanRestores.fetch_add(1,std::memory_order_relaxed);
                                    }
                                } else {
                                    GroundTextCyanWriteGuards.fetch_add(1,std::memory_order_relaxed);
                                    glyphTarget=nullptr;
                                }
                                GroundTextCyanWriteMutex.unlock();
                            }
                        } else if (cyan) {
                            GroundTextCyanColorRejects.fetch_add(1,std::memory_order_relaxed);
                        }
                    }
                }
            }
        }
        if (tint && ground && !concealGroundVisuals) {
            if (!paired || !colorOk || !idOk || !textOk) {
                BackgroundGuardFailures.fetch_add(1,std::memory_order_relaxed);
            } else if (!identified) {
                BackgroundRuleNoIdentity.fetch_add(1,std::memory_order_relaxed);
                BackgroundPaintIdRejected.fetch_add(1,std::memory_order_relaxed);
            } else {
                BackgroundPaintIdQualified.fetch_add(1,std::memory_order_relaxed);
                const auto snapshot=std::atomic_load_explicit(&PublishedFilterRules,
                    std::memory_order_acquire);
                const auto ruleItem=CachedGroundRuleItem(verifiedCode,verifiedClassId,
                    verifiedQuantity,snapshot.get(),&verifiedProperties);
                GroundRuleDecision resolvedRule{};
                const auto* rule=ResolveGroundRule(snapshot.get(),ruleItem,resolvedRule) ?
                    &resolvedRule : nullptr;
                if (!rule || !rule->hasBackground) {
                    BackgroundRuleNoColor.fetch_add(1,std::memory_order_relaxed);
                } else if (rule->hasName &&
                    !GroundQuantity::MatchesRuleName(
                        std::string_view(textBytes.data(),nameLength),
                        std::string_view(rule->name.data(),rule->bytes-1U),
                        verifiedQuantity)) {
                    BackgroundPaintNameRejected.fetch_add(1,std::memory_order_relaxed);
                } else if (std::memcmp(rgba.data(),OriginalGroundBackground.data(),
                    sizeof(rgba))!=0) {
                    BackgroundPaintColorRejected.fetch_add(1,std::memory_order_relaxed);
                } else {
                    forwardedRgba=rule->background;
                    forwardedColor=forwardedRgba.data();
                    customForwarded=true;
                    BackgroundRuleForwarded.fetch_add(1,std::memory_order_relaxed);
                    // Keep the old counter visible for 0.1.30 comparisons.
                    BackgroundPaintForwardedPurple.fetch_add(1,std::memory_order_relaxed);
                }
            }
        }
        if (observe && colorOk) {
            const bool originalPurple=std::memcmp(rgba.data(),
                PurpleDivineBackground.data(),sizeof(rgba))==0;
            const bool originalBlack=std::memcmp(rgba.data(),
                OriginalGroundBackground.data(),sizeof(rgba))==0;
            BackgroundPaintSample* dest=&BackgroundPaintLastOther;
            if (divine) {
                BackgroundPaintDivineCalls.fetch_add(1,std::memory_order_relaxed);
                if (originalPurple) BackgroundPaintDivinePurple.fetch_add(1,std::memory_order_relaxed);
                if (originalBlack) BackgroundPaintDivineBlack.fetch_add(1,std::memory_order_relaxed);
                dest=&BackgroundPaintLastDivine;
            } else if (map) {
                BackgroundPaintMapCalls.fetch_add(1,std::memory_order_relaxed);
                if (originalPurple) BackgroundPaintMapPurple.fetch_add(1,std::memory_order_relaxed);
                if (originalBlack) BackgroundPaintMapBlack.fetch_add(1,std::memory_order_relaxed);
                dest=&BackgroundPaintLastMap;
            } else BackgroundPaintOtherCalls.fetch_add(1,std::memory_order_relaxed);
            if (BackgroundPaintSampleMutex.try_lock()) {
                dest->rect=recordAddress;
                dest->text=textAddress;
                dest->color=colorAddress;
                dest->recordId=idOk?recordId:0;
                dest->rgba=rgba;
                dest->forwardedPurple=customForwarded;
                dest->name.fill(0);
                if (textOk) std::memcpy(dest->name.data(),textBytes.data(),
                    dest->name.size()-1);
                ++dest->hits;
                dest->valid=true;
                BackgroundPaintSampleMutex.unlock();
            } else BackgroundPaintLockContention.fetch_add(1,std::memory_order_relaxed);
        }
        if (ground && identified &&
            (customForwarded || hasScopedRuleTextColor)) {
            latencyBulkBackgroundForwarded=customForwarded;
            latencyBulkCode=verifiedCode;
            latencyBulkRecordId=recordId;
        }
    }
    // Keep native label record and paint execution intact. Only the color
    // arguments passed into the synchronous native draw are transparent.
    // No persistent writes to native objects, geometry or hover state.
    std::array<float,4> invisibleRgba{{0.f,0.f,0.f,0.f}};
    if (concealGroundVisuals) {
        forwardedColor=invisibleRgba.data();
        HiddenGroundPaints.fetch_add(1,std::memory_order_relaxed);
    }
    const float* const previousGlyphColor=VerifiedRuleGlyphColor;
    // The qualified glyph-B hook consumes the same scoped color pointer.
    // Hidden labels take precedence over configured textColor; the renderer
    // still runs and receives zero alpha for each glyph. No key-state gate.
    VerifiedRuleGlyphColor=concealGroundVisuals ? invisibleRgba.data() :
        (ground && hasScopedRuleTextColor?scopedRuleTextColor.data():nullptr);
    // 0.2.23 painter-suppression trial (IN-GAME PICKUP TEST FAILED):
    // not calling this renderer did NOT prevent clicking the hidden ground
    // label's former location. 0.2.10/0.2.11 only established that skipping
    // the painter changed the visible hover response; that did not prove
    // ownership of native hit testing or pickup. 0.2.25 records independent
    // bulk/hover draw suppression counters before touching any selector.
    //
    // Skip ONLY the already fully qualified bulk-ground painter invocation for
    // a JSON show:false rule. Do not mutate the item, unit flags, ownership,
    // record geometry, inventory state, or global input handling. Visible items
    // and non-ground UI continue to forward the original exactly once.
    if (concealGroundVisuals) {
        HiddenGroundLastPainterSkipId.store(concealedRecordId,std::memory_order_relaxed);
        HiddenGroundLastPainterSkipCode.store(concealedVerifiedCode,std::memory_order_relaxed);
        HiddenGroundLastPainterSkipMs.store(GetTickCount64(),std::memory_order_release);
        HiddenGroundInteractionPainterSkips.fetch_add(1,std::memory_order_relaxed);
    } else if (OriginalSharedLabelPaint) {
        OriginalSharedLabelPaint(rect,textArg,forwardedColor);
    }
    VerifiedRuleGlyphColor=previousGlyphColor;
    // Legacy record-write observer only (disabled by default); not part of the
    // JSON textColor path. It never runs merely because textColor is enabled.
    if (glyphWasCyanOrPatched && glyphTarget) {
        std::array<std::uint32_t,4> after{};
        SIZE_T afterCopied{};
        if (ReadProcessMemory(GetCurrentProcess(),glyphTarget,after.data(),
                sizeof(after),&afterCopied) && afterCopied==sizeof(after)) {
            if (after==ProbeCyanGlyphColor)
                GroundTextCyanAfterOriginal.fetch_add(1,std::memory_order_relaxed);
            else if (after==NativeDivineGlyphColor)
                GroundTextNativeAfterOriginal.fetch_add(1,std::memory_order_relaxed);
        } else GroundTextCyanAfterReadFailures.fetch_add(1,std::memory_order_relaxed);
    }
}


// 0.1.42 crash diagnosis: glyph-B is NOT the ABI of SoE ImageWidget submit at 0x858510.
// Original call D2R+0x90857B prepares RCX=context, XMM1=first scalar,
// XMM2=second scalar and R9=&stack-local float[4]. At B+0x32 B moves
// R9 into RDI and at B+0x141 reads [RDI]. The old 0.1.40 hook declared
// fourth argument float/XMM3; its forwarder corrupted the R9 color pointer.
// Only B is hooked here; 0x858510 is the SoE-owned ImageWidget submit hook.
using CorrectedGlyphBFn=std::uint64_t (__fastcall*)(
    void*,float,float,const float*) noexcept;
CorrectedGlyphBFn OriginalCorrectedGlyphB{};
std::atomic_bool CorrectedGlyphBInstalled{};
std::atomic<std::uint64_t> CorrectedGlyphBCalls{};
std::atomic<std::uint64_t> CorrectedGlyphBScoped{};
std::atomic<std::uint64_t> CorrectedGlyphBRuleColorForwarded{};
std::atomic<std::uint64_t> CorrectedGlyphBInvalidColors{};
std::atomic<std::uint64_t> CorrectedGlyphBReadFailures{};
std::atomic<std::uint64_t> CorrectedGlyphBUnscoped{};
constexpr std::uintptr_t CorrectedGlyphBRva=0x658510;
constexpr std::uintptr_t CorrectedGlyphBCallRva=0x90857B;

// Automatic activation for every game after SoE V1/V3 interop is attached.
// Fail closed on build/fingerprint/API mismatch: no global UI fallback.
void EnableAutomaticNativeHover() noexcept {
    const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    const auto* scope=InWorldRenderScopeApi.load(std::memory_order_acquire);
    if (!rules || (!rules->backgroundRules && !rules->hiddenRules) ||
        ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules ||
        InWorldMode.load(std::memory_order_acquire)!=InWorldBackend::SoEInterop ||
        !scope || !scope->getCurrentItem || !scope->isReady ||
        !scope->isReady() || !OriginalGetItemCode || !HookInstalled.load()) {
        Emit("LOOT_NATIVE_HOVER_UNAVAILABLE reason=json-or-SoE-V3-or-native-filter-not-ready no-fallback=1");
        return;
    }
    if (!NativeRowAppendHookInstalled.load(std::memory_order_acquire) ||
        !NativeRowRendererHookInstalled.load(std::memory_order_acquire))
        ArmNativeRowRuntime();
    if (!NativeRowAppendHookInstalled.load(std::memory_order_acquire) ||
        !NativeRowRendererHookInstalled.load(std::memory_order_acquire) ||
        !OriginalNativeRowAppend || !OriginalNativeRowRenderer) {
        Emit("LOOT_NATIVE_HOVER_UNAVAILABLE native-append-or-renderer-hook-refused no-fallback=1");
        return;
    }
    bool pairedTextRule=false;
    for (const auto& rule:rules->rules)
        if (rule.hasBackground && rule.hasTextColor &&
            NativeRowFontColorPolicy::ValidJsonColor(rule.textColor)) {
            pairedTextRule=true;
            break;
        }
    // Both switches become live automatically. A rule with textColor but no
    // backgroundColor deliberately cannot bypass row ownership qualification.
    const bool fontReady=pairedTextRule &&
        CorrectedGlyphBInstalled.load(std::memory_order_acquire) &&
        OriginalCorrectedGlyphB;
    // Font may be omitted (no paired text rule). Report an unavailable
    // glyph hook when text rules were requested; background stays active.
    if (pairedTextRule && !fontReady)
        Emit("LOOT_NATIVE_HOVER_UNAVAILABLE font-glyph-hook-not-ready background-only=1");
    NativeRowBgLiveEnabled.store(true,std::memory_order_release);
    NativeRowFontColorEnabled.store(fontReady,std::memory_order_release);
    char line[230]{};
    std::snprintf(line,sizeof(line),
        "LOOT_NATIVE_HOVER_READY version=1.0.0 background=%u font=%u hiddenRules=%zu rules=%zu "
        "hide-hover=qualified-native-row-suppression globalRect=0 captureFile=0 consoleCommands=0",
        rules->backgroundRules?1U:0U,fontReady?1U:0U,
        rules->hiddenRules,rules->rules.size());
    Emit(line);
}

// Native font override: only inside the SAME synchronous qualified native hidden-hover row
// renderer invocation as the already-proven background substitution. The
// item code, append sequence and color are copied before the draw into TLS;
// no stale element pointer, textual/coordinate guess, or global UI tint.
// Preserve native alpha and never modify native glyph/color memory.
bool TryForwardNativeRowFontGlyph(void* context,float x,float y,
    const float* nativeRgba,std::uintptr_t caller,
    std::uint64_t& result) noexcept {
    if (!NativeRowFontColorEnabled.load(std::memory_order_acquire))
        return false;
    const auto& draw=NativeRowFontCurrentDraw;
    if (!draw.active || !NativeRowBgInsideRenderer || !draw.unitId ||
        !draw.appendSequence || !NativeRowBgLiveEnabled.load(
            std::memory_order_acquire)) return false;
    NativeRowFontColorAttempts.fetch_add(1,std::memory_order_relaxed);
    if (caller!=Base+CorrectedGlyphBCallRva+5 || VerifiedRuleGlyphColor) {
        NativeRowFontColorRejectedCaller.fetch_add(1,
            std::memory_order_relaxed);
        return false;
    }
    if (!draw.sessionEpoch ||
        draw.sessionEpoch!=NativeRowBgLiveEpoch.load(
            std::memory_order_acquire)) {
        NativeRowFontColorRejectedEpoch.fetch_add(1,
            std::memory_order_relaxed);
        return false;
    }
    std::array<float,4> configured{};
    std::memcpy(configured.data(),draw.configuredGlyphBits.data(),
        sizeof(configured));
    if (!draw.hasTextRule ||
        !NativeRowFontColorPolicy::ValidJsonColor(configured)) {
        NativeRowFontColorRejectedRule.fetch_add(1,
            std::memory_order_relaxed);
        return false;
    }
    std::array<float,4> native{};
    SIZE_T read{};
    if (!nativeRgba ||
        !ReadProcessMemory(GetCurrentProcess(),nativeRgba,native.data(),
            sizeof(native),&read) || read!=sizeof(native)) {
        NativeRowFontColorReadFailures.fetch_add(1,
            std::memory_order_relaxed);
        return false;
    }
    // Fail closed on colored native text/shadows/other sub-elements.
    // Only the plain near-white label observed in the 0.2.2 capture is
    // eligible; no native palette or alpha is globally replaced.
    if (!NativeRowFontColorPolicy::VanillaGroundLabel(native)) {
        NativeRowFontColorRejectedNative.fetch_add(1,
            std::memory_order_relaxed);
        return false;
    }
    configured[3]=native[3]; // RGBA JSON RGB only; preserve engine alpha.
    if (!NativeRowFontColorEnabled.load(std::memory_order_acquire) ||
        !NativeRowBgLiveEnabled.load(std::memory_order_acquire) ||
        draw.sessionEpoch!=NativeRowBgLiveEpoch.load(
            std::memory_order_acquire)) {
        NativeRowFontColorRejectedEpoch.fetch_add(1,
            std::memory_order_relaxed);
        return false;
    }
    // Stack float4 remains alive until the original glyph call returns.
    // Forward exactly once; this does not write to native item, UI row,
    // glyph context or the original R9 color pointer.
    result=OriginalCorrectedGlyphB(context,x,y,configured.data());
    NativeRowFontColorForwarded.fetch_add(1,
        std::memory_order_relaxed);
    NativeRowFontColorLastUnitId.store(draw.unitId,
        std::memory_order_relaxed);
    NativeRowFontColorLastCode.store(draw.code,
        std::memory_order_relaxed);
    NativeRowFontColorLastAppendSeq.store(draw.appendSequence,
        std::memory_order_relaxed);
    std::array<std::uint32_t,4> beforeBits{},forwardedBits{};
    std::memcpy(beforeBits.data(),native.data(),sizeof(beforeBits));
    std::memcpy(forwardedBits.data(),configured.data(),sizeof(forwardedBits));
    for (unsigned i=0;i<4;++i) {
        NativeRowFontColorLastOriginalBits[i].store(beforeBits[i],
            std::memory_order_relaxed);
        NativeRowFontColorLastForwardedBits[i].store(forwardedBits[i],
            std::memory_order_relaxed);
    }
    return true;
}

std::uint64_t __fastcall HookCorrectedGlyphB(
    void* context,float x,float y,const float* nativeRgba) noexcept {
    if (!OriginalCorrectedGlyphB) return 0;
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    std::uint64_t colored{};
    if (TryForwardNativeRowFontGlyph(context,x,y,nativeRgba,caller,colored))
        return colored;
    // Preserve the existing Alt-visible JSON textColor route unchanged.
    if (caller!=Base+CorrectedGlyphBCallRva+5 ||
        !CorrectedGlyphBArmed.load(std::memory_order_relaxed) ||
        !VerifiedRuleGlyphColor || !nativeRgba)
        return OriginalCorrectedGlyphB(context,x,y,nativeRgba);
    std::array<float,4> before{};
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(),nativeRgba,before.data(),
            sizeof(before),&copied) || copied!=sizeof(before))
        return OriginalCorrectedGlyphB(context,x,y,nativeRgba);
    for (const auto value:before)
        if (!std::isfinite(value) || value<0.f || value>1.001f)
            return OriginalCorrectedGlyphB(context,x,y,nativeRgba);
    return OriginalCorrectedGlyphB(context,x,y,VerifiedRuleGlyphColor);
}

void ArmCorrectedGlyphB(bool observeOnly=false) noexcept {
    if (CorrectedGlyphBInstalled.load(std::memory_order_acquire)) {
        if(!observeOnly) CorrectedGlyphBArmed.store(true,std::memory_order_release);
        Emit(observeOnly?
            "LOOT_UI_TEXT_OBSERVER_READY version=1.0.0 already-installed=1 glyphB=0x658510 glyphA=untouched ruleColors=unchanged":
            "LOOT_GLYPH_B_ARMED version=1.0.0 already-installed=1 abi=RCX,XMM1,XMM2,R9-pointer hookA=0");
        return;
    }
    const char* build=Context ? D2RL::GetBuildName(Context) : nullptr;
    if (!Context || !build || std::string_view(build)!="93847" ||
        (!observeOnly &&
         (!FormatterHookInstalled.load(std::memory_order_acquire) ||
          !InnerNameHookInstalled.load(std::memory_order_acquire) ||
          !BackgroundPaintHookInstalled.load(std::memory_order_acquire) ||
          ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules))) {
        Emit("LOOT_GLYPH_B_REFUSED version=1.0.0 reason=build-or-required-filter-hook-missing hooks=0");
        return;
    }
    constexpr std::array<std::uint8_t,5> nativeCall{0xE8,0x90,0xFF,0xD4,0xFF};
    // First 16 native B entry bytes from the 0.1.42 unhooked dump.
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
        !ReadSafe(0x908525,prep.data(),prep.size()) || prep!=colorPointerPrep ||
        !Context->CheckExpectedBytes(CorrectedGlyphBRva,nativeEntry.data(),
            static_cast<std::uint32_t>(nativeEntry.size()))) {
        Emit("LOOT_GLYPH_B_REFUSED version=1.0.0 reason=callsite-or-R9-or-entry-fingerprint-mismatch hooks=0");
        return;
    }
    if (!Context->InstallInlineHook(CorrectedGlyphBRva,nativeEntry.data(),
        static_cast<std::uint32_t>(nativeEntry.size()),HookCorrectedGlyphB,
        &OriginalCorrectedGlyphB) || !OriginalCorrectedGlyphB) {
        Emit("LOOT_GLYPH_B_REFUSED version=1.0.0 reason=loader-hook-install-failed hooks=0");
        return;
    }
    CorrectedGlyphBInstalled.store(true,std::memory_order_release);
    if(!observeOnly) CorrectedGlyphBArmed.store(true,std::memory_order_release);
    Emit(observeOnly?
        "LOOT_UI_TEXT_OBSERVER_READY version=1.0.0 target=D2R+0x658510 caller=dynamic ABI=RCX,XMM1,XMM2,R9-pointer glyphA=untouched ruleColors=unchanged itemWrites=0":
        "LOOT_GLYPH_B_ARMED version=1.0.0 target=D2R+0x658510 caller=D2R+0x90857B ABI=RCX-glyph,XMM1-float,XMM2-float,R9-float4-pointer onlyB=1 hookA=0 recordWrites=0 itemWrites=0");
}

void ArmBackgroundPaintObservation() noexcept;

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
        ArmBackgroundPaintObservation();
    if (!BackgroundPaintHookInstalled.load(std::memory_order_acquire) ||
        !OriginalSharedLabelPaint) {
        CorrectedGlyphBArmed.store(false,std::memory_order_release);
        Emit("LOOT_TEXT_COLOR_RULES_REFUSED reason=shared-paint-hook-missing");
        return;
    }
    ArmCorrectedGlyphB();
    if (CorrectedGlyphBArmed.load(std::memory_order_acquire)) {
        char message[260]{};
        std::snprintf(message,sizeof(message),
            "LOOT_TEXT_COLOR_RULES_ARMED version=1.0.0 generation=%llu textColorRules=%zu source=runtime-json glyphB=only ABI=RCX,XMM1,XMM2,R9-pointer itemWrites=0",
            static_cast<unsigned long long>(snapshot->generation),snapshot->textColorRules);
        Emit(message);
    }
}

void ReportCorrectedGlyphB() noexcept {
    char line[400]{};
    std::snprintf(line,sizeof(line),
        "LOOT_GLYPH_B_STATUS version=1.0.0 installed=%u armed=%u calls=%llu scoped=%llu ruleColorForwarded=%llu nativeForwarded=%llu invalidColor=%llu readFailure=%llu onlyB=1 hookA=0 ABI=RCX,XMM1,XMM2,R9-pointer",
        CorrectedGlyphBInstalled.load()?1U:0U,
        CorrectedGlyphBArmed.load()?1U:0U,
        static_cast<unsigned long long>(CorrectedGlyphBCalls.load()),
        static_cast<unsigned long long>(CorrectedGlyphBScoped.load()),
        static_cast<unsigned long long>(CorrectedGlyphBRuleColorForwarded.load()),
        static_cast<unsigned long long>(CorrectedGlyphBUnscoped.load()),
        static_cast<unsigned long long>(CorrectedGlyphBInvalidColors.load()),
        static_cast<unsigned long long>(CorrectedGlyphBReadFailures.load()));
    Emit(line);
}

void ArmBackgroundPaintObservation() noexcept {
    if (BackgroundPaintHookInstalled.load(std::memory_order_acquire)) {
        BackgroundPaintObserveArmed.store(true,std::memory_order_release);
        Emit("LOOT_BACKGROUND_PAINT_OBSERVER_READY already-installed=1 observe=1");
        return;
    }
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
        !Context->CheckExpectedBytes(0x1FA8E0,expected.data(),
            static_cast<std::uint32_t>(expected.size())) ||
        !ReadSafe(0x1517AF1,groundBytes.data(),groundBytes.size()) ||
        groundBytes!=groundCall ||
        !ReadSafe(0x1519E41,neighborBytes.data(),neighborBytes.size()) ||
        neighborBytes!=neighborCall) {
        Emit("LOOT_BACKGROUND_PAINT_OBSERVER_REFUSED build-or-callsite-or-entry-fingerprint-mismatch; no-hook=1");
        return;
    }
    if (!Context->InstallInlineHook(0x1FA8E0,expected.data(),
        static_cast<std::uint32_t>(expected.size()),HookSharedLabelPaint,
        &OriginalSharedLabelPaint) || !OriginalSharedLabelPaint) {
        Emit("LOOT_BACKGROUND_PAINT_OBSERVER_REFUSED loader-install-failed; no-fallback=1");
        return;
    }
    BackgroundPaintHookInstalled.store(true,std::memory_order_release);
    BackgroundPaintObserveArmed.store(true,std::memory_order_release);
    Emit("LOOT_BACKGROUND_PAINT_OBSERVER_READY version=1.0.0 hook=D2R+0x1FA8E0 returnSites=D2R+0x1517AF6,D2R+0x1519E46 nativeRGBA-read-only opt-in-argument-override original-forwarded-once=1");
}

void ReportBackgroundPaintStatus() noexcept {
    char message[800]{};
    std::snprintf(message,sizeof(message),
        "LOOT_BACKGROUND_PAINT_STATUS version=1.0.0 armed=%u installed=%u total=%llu ground=%llu neighbor=%llu divine=%llu divinePurple=%llu divineBlack=%llu map=%llu mapPurple=%llu mapBlack=%llu other=%llu failures=%llu contended=%llu forwardedColor=%llu idQualified=%llu idRejected=%llu nameRejected=%llu colorRejected=%llu",
        BackgroundPaintObserveArmed.load()?1U:0U,
        BackgroundPaintHookInstalled.load()?1U:0U,
        static_cast<unsigned long long>(BackgroundPaintCalls.load()),
        static_cast<unsigned long long>(BackgroundPaintGroundCalls.load()),
        static_cast<unsigned long long>(BackgroundPaintNeighborCalls.load()),
        static_cast<unsigned long long>(BackgroundPaintDivineCalls.load()),
        static_cast<unsigned long long>(BackgroundPaintDivinePurple.load()),
        static_cast<unsigned long long>(BackgroundPaintDivineBlack.load()),
        static_cast<unsigned long long>(BackgroundPaintMapCalls.load()),
        static_cast<unsigned long long>(BackgroundPaintMapPurple.load()),
        static_cast<unsigned long long>(BackgroundPaintMapBlack.load()),
        static_cast<unsigned long long>(BackgroundPaintOtherCalls.load()),
        static_cast<unsigned long long>(BackgroundPaintReadFailures.load()),
        static_cast<unsigned long long>(BackgroundPaintLockContention.load()),
        static_cast<unsigned long long>(BackgroundPaintForwardedPurple.load()),
        static_cast<unsigned long long>(BackgroundPaintIdQualified.load()),
        static_cast<unsigned long long>(BackgroundPaintIdRejected.load()),
        static_cast<unsigned long long>(BackgroundPaintNameRejected.load()),
        static_cast<unsigned long long>(BackgroundPaintColorRejected.load()));
    Emit(message);
    std::lock_guard lock(BackgroundPaintSampleMutex);
    const std::array<std::pair<const char*,const BackgroundPaintSample*>,3> samples{{
        {"divine",&BackgroundPaintLastDivine},
        {"map",&BackgroundPaintLastMap},
        {"other",&BackgroundPaintLastOther}
    }};
    for (const auto& [kind,sample]:samples) {
        if (!sample->valid) continue;
        char name[64]{};
        for (std::size_t i=0;i<sample->name.size()-1 && sample->name[i];++i) {
            const auto c=sample->name[i];
            name[i]=(c=='\n'||c=='\r')?'|':c;
        }
        char line[450]{};
        std::snprintf(line,sizeof(line),
            "LOOT_BACKGROUND_PAINT_SAMPLE type=%s hits=%llu rect=0x%llX colorPtr=0x%llX unitId=%u nativeRGBA=%.3f,%.3f,%.3f,%.3f forwardedColor=%u text='%s'",
            kind,static_cast<unsigned long long>(sample->hits),
            static_cast<unsigned long long>(sample->rect),
            static_cast<unsigned long long>(sample->color),
            sample->recordId,sample->rgba[0],sample->rgba[1],sample->rgba[2],
            sample->rgba[3],sample->forwardedPurple?1U:0U,name);
        Emit(line);
    }
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
        ArmBackgroundPaintObservation();
    if (!BackgroundPaintHookInstalled.load(std::memory_order_acquire) ||
        !OriginalSharedLabelPaint || !VerifyBackgroundColorRoute()) {
        BackgroundTintArmed.store(false,std::memory_order_release);
        Emit("LOOT_BACKGROUND_RULES_REFUSED native-paint-hook-or-fingerprint-mismatch names-remain-active=1");
        return;
    }
    BackgroundTintEverArmed.store(true,std::memory_order_release);
    BackgroundTintArmed.store(true,std::memory_order_release);
    char message[260]{};
    std::snprintf(message,sizeof(message),
        "LOOT_BACKGROUND_RULES_ARMED version=1.0.0 generation=%llu coloredRules=%zu source=runtime-json paint=argument-forward-only recordWrites=0 itemWrites=0",
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
        ArmBackgroundPaintObservation();
    // Hiding requires both visual channels. A color-only mask would leave
    // unstyled native glyphs visible and must fail open instead.
    if (BackgroundPaintHookInstalled.load(std::memory_order_acquire) &&
        OriginalSharedLabelPaint && VerifyBackgroundColorRoute() &&
        !CorrectedGlyphBArmed.load(std::memory_order_acquire))
        ArmCorrectedGlyphB();
    if (!BackgroundPaintHookInstalled.load(std::memory_order_acquire) ||
        !OriginalSharedLabelPaint || !VerifyBackgroundColorRoute() ||
        !CorrectedGlyphBInstalled.load(std::memory_order_acquire) ||
        !OriginalCorrectedGlyphB ||
        !CorrectedGlyphBArmed.load(std::memory_order_acquire)) {
        Emit("LOOT_VISIBILITY_REFUSED native-painter-or-glyph-unavailable show:false-fails-open=1");
        return;
    }
    HideGroundArmed.store(true,std::memory_order_release);
    char message[250]{};
    std::snprintf(message,sizeof(message),
        "LOOT_VISIBILITY_READY version=1.0.0 hiddenRules=%zu source=JSON-show:false bulk-ground-native-painter-skip native-painter-forwarded=0-for-qualified-hidden-only key-state-independent=1 hover-native-path=qualified-hidden-row-skip pickup-state-writes=0",
        rules->hiddenRules);
    Emit(message);
}


// Per-game first-observed-unit sound alert. The Alt-ground-label and optional
// SoE / standalone hidden-hover routes feed one registry. The public
// InventoryService is polled on the UI thread: seeing an existing, alerted
// unit ID in the player's owned inventory or cursor FORGETS precisely that ID.
// A later ground observation of the same unit can play another sound. This
// does not hook monster/TC drops or infer pickup from missing labels.
// Sound resolution still uses the qualified D2RLoader sounds.txt row-NAME
// backend on the game thread, not the sounds.txt numeric Index or FMOD.
const D2RL::ThreadServiceV1* SoundThreads{};
const D2RL::InventoryServiceV1* SoundInventory{};
const D2RL::ItemServiceV1* SoundInventoryItems{};
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
std::atomic<std::uint64_t> SoundObservedHidden{};
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
            "LOOT_COMPAT_SOUND_LOADER version=1.0.0 peRead=%u "
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
                "LOOT_COMPAT_SOUND_WITNESS version=1.0.0 rva=0x%X "
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
            "LOOT_SOUND_QUALIFY matched=1 version=1.0.0 "
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
    bool automatic{},hiddenHover{};
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
        "LOOT_SOUND_PLAY version=1.0.0 trigger=%s code='%s' unitId=%u name='%s' engineReturned=%u gameThread=%lu note=return-not-audibility-proof",
        request->automatic?(request->hiddenHover?"first-hidden-hover":"first-alt-ground-label"):"manual-test",codeText,request->unitId,
        request->name.data(),result?1U:0U,static_cast<unsigned long>(GetCurrentThreadId()));
    if(logger) {
        logger->LogInfo(message);
    }
}

bool QueueNamedSound(std::string_view name,std::uint32_t unitId,
                     std::uint32_t code,bool automatic,
                     bool hiddenHover=false,
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
    request->hiddenHover=hiddenHover;
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
    std::uint32_t rawCode, bool hiddenHover,
    const void* nativeUnit,std::uint32_t knownClassId) noexcept {
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
    if (hiddenHover) SoundObservedHidden.fetch_add(1,std::memory_order_relaxed);
    else SoundObservedAlt.fetch_add(1,std::memory_order_relaxed);
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
        hiddenHover,observedEpoch,itemTicket);
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
    ObserveGroundSoundIdentity(header[2],OriginalGetItemCode(unit),false,unit,header[1]);
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
            "LOOT_SOUND_PICKUP_REARM version=1.0.0 cleared=%u lastUnitId=%u "
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
    const D2RL::InventoryServiceV1* inventory{};
    if (Context->QueryService(D2RL::ServiceId::Inventory,
            D2RL::InventoryServiceV1Version,&inventory) !=
            D2RL::ServiceQueryResult::Success ||
        !D2RL::HasInventoryServiceV1Field(inventory,
            D2RL::InventoryServiceV1RequiredSize) ||
        !inventory->getLocalPlayer || !inventory->forEachInventoryItem) {
        Emit("LOOT_SOUND_PICKUP_OBSERVER_UNAVAILABLE reason=public-inventory-service-missing "
             "sounds-continue=1 same-id-redrop=not-rearmed");
        return;
    }
    SoundInventory=inventory;
    const D2RL::ItemServiceV1* items{};
    if (Context->QueryService(D2RL::ServiceId::Item,
            D2RL::ItemServiceV1Version,&items) ==
            D2RL::ServiceQueryResult::Success &&
        D2RL::HasItemServiceV1Field(items,D2RL::ItemServiceV1RequiredSize) &&
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
        "LOOT_SOUND_PICKUP_OBSERVER_READY version=1.0.0 source=SDK-InventoryService "
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
            "LOOT_COMPAT_SOUND_GUARD version=1.0.0 threadService=%u "
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
        "LOOT_SOUND_ARMED version=1.0.0 mode=session-unit-id-first-observed sources=alt-ground-label+hidden-hover soundRules=%zu baselineMs=%llu no-visibility-replay=1 same-id-redrop=rearm-on-inventory-observation no-item-writes=1 note=not-native-drop-event",
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
        NativeRowBgLiveEnabled.store(false,std::memory_order_release);
        NativeRowFontColorEnabled.store(false,std::memory_order_release);
        NativeRowBgLiveEpoch.fetch_add(1,std::memory_order_acq_rel);
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
    if (InWorldMode.load(std::memory_order_acquire)==InWorldBackend::SoEInterop) {
        if (rules->backgroundRules || rules->hiddenRules)
            EnableAutomaticNativeHover();
        else {
            NativeRowBgLiveEnabled.store(false,std::memory_order_release);
            NativeRowFontColorEnabled.store(false,std::memory_order_release);
            NativeRowBgLiveEpoch.fetch_add(1,std::memory_order_acq_rel);
        }
    }
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
        "LOOT_FILTER_AUTO_ACTIVE version=1.0.0 trigger=%s generation=%llu rules=%zu nameRules=%zu hiddenRules=%zu backgrounds=%u textColors=%u sound=%u soeInterop=independent hiddenHoverStyle=optional-soe-v2",
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
    // Only the live render-thread identity epoch is invalidated. The ground
    // item and the native label renderer remain owned by D2R/SoE.
    NativeRowBgLiveEpoch.fetch_add(1,std::memory_order_acq_rel);
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
        "LOOT_RELOAD_OK version=1.0.0 trigger=%s previousGeneration=%llu "
        "generation=%llu schema=%u rules=%zu hiddenRules=%zu "
        "backgroundRules=%zu textColorRules=%zu soundRules=%zu "
        "pickupCallerPolicy=none captureRequired=0",
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

void ReportGroundSoundStatus() noexcept {
    auto rules=std::atomic_load_explicit(&PublishedFilterRules,std::memory_order_acquire);
    char message[1050]{};
    std::snprintf(message,sizeof(message),
        "LOOT_SOUND_STATUS version=1.0.0 armed=%u soundRules=%zu nativeBackend=%u threadService=%u registryEpoch=%llu seen=%llu baseline=%llu newCandidates=%llu queued=%llu nativeReturned=%llu nativeUnknown=%llu cancelled=%llu queueRejected=%llu cacheContended=%llu cacheFull=%llu alreadySeen=%llu altObserved=%llu hiddenObserved=%llu pickupObserver=%u pickupPolls=%llu pickupPollOK=%llu pickupResets=%llu pickupBusy=%llu pickupQueueRejected=%llu mode=first-observed-with-inventory-pickup-rearm not-native-drop-event",
        SoundArmed.load()?1U:0U,rules?rules->soundRules:0,
        SoundLoaderBase.load()?1U:0U,SoundThreads?1U:0U,
        static_cast<unsigned long long>(SoundRegistryEpoch.load()),
        static_cast<unsigned long long>(SoundSeenTotal.load()),
        static_cast<unsigned long long>(SoundBaselineTotal.load()),
        static_cast<unsigned long long>(SoundQualifiedNew.load()),
        static_cast<unsigned long long>(SoundQueued.load()),
        static_cast<unsigned long long>(SoundPlayed.load()),
        static_cast<unsigned long long>(SoundUnknown.load()),
        static_cast<unsigned long long>(SoundCancelled.load()),
        static_cast<unsigned long long>(SoundQueueRejected.load()),
        static_cast<unsigned long long>(SoundCacheContention.load()),
        static_cast<unsigned long long>(SoundCacheFull.load()),
        static_cast<unsigned long long>(SoundAlreadySeen.load()),
        static_cast<unsigned long long>(SoundObservedAlt.load()),
        static_cast<unsigned long long>(SoundObservedHidden.load()),
        SoundInventory?1U:0U,
        static_cast<unsigned long long>(SoundPickupPolls.load()),
        static_cast<unsigned long long>(SoundPickupPollSuccess.load()),
        static_cast<unsigned long long>(SoundPickupResets.load()),
        static_cast<unsigned long long>(SoundPickupBusy.load()),
        static_cast<unsigned long long>(SoundPickupPollRejected.load()));
    Emit(message);
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
    Emit("LOOT_FORMATTER_OBSERVER_READY version=1.0.0 hook=D2R+0x1FA9F0 sourceCalls=D2R+0x15171E5,D2R+0x1517783 expectedReturnRvas=0x15171EA,0x1517788 sixArgsForwarded=1 labelWrites=none nativeCodeWrites=loader-managed-hook-only");
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
        Emit("LOOT_PICKUP_GUARD_INACTIVE version=1.0.0 reason=wrong-build-or-image forward-only=1");
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
        Emit("LOOT_PICKUP_GUARD_INACTIVE version=1.0.0 reason=PE-timestamp-or-image-size-mismatch forward-only=1");
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
            Emit("LOOT_PICKUP_GUARD_INACTIVE version=1.0.0 reason=dispatch-callsite-witness-mismatch forward-only=1");
            return;
        }
    }
    constexpr std::array<std::uint8_t,3> lookupEntry{{0x4C,0x63,0xCA}};
    if(!NativeCallMatches(0xFACB9,NativeItemLookupRva) ||
       !Context->CheckExpectedBytes(NativeItemLookupRva,
            lookupEntry.data(),static_cast<std::uint32_t>(lookupEntry.size()))){
        Emit("LOOT_PICKUP_GUARD_INACTIVE version=1.0.0 reason=item-lookup-witness-mismatch forward-only=1");
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
        Emit("LOOT_PICKUP_GUARD_INACTIVE version=1.0.0 reason=dispatch-entry-unqualified forward-only=1");
        return;
    }

    if(!Context->InstallInlineHook(NativeActionDispatchRva,
            dispatchEntry.data(),static_cast<std::uint32_t>(dispatchEntry.size()),
            HookNativeActionDispatch,&OriginalNativeActionDispatch) ||
       !OriginalNativeActionDispatch){
        Emit("LOOT_PICKUP_GUARD_INACTIVE version=1.0.0 reason=dispatch-hook-registration-failed forward-only=1");
        return;
    }

    NativeActionDispatchInstalled.store(true,std::memory_order_release);
    NativePickupGuardQualified.store(true,std::memory_order_release);
    Emit("LOOT_PICKUP_GUARD_READY version=1.0.0 action=22 type=4 mode=3 "
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
                Emit("LOOT_RELOAD_REQUEST version=1.0.0 trigger=stable-file-change settleMs=650");
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
                    Emit("LOOT_RELOAD_REQUEST version=1.0.0 trigger=stable-excel-change settleMs=650");
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

        // The only production diagnostic-style hotkey retained is explicit
        // manual configuration reload. It does not consume the game input.
        const bool reloadChord=(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0 &&
            (GetAsyncKeyState(VK_SHIFT)&0x8000)!=0 &&
            (GetAsyncKeyState(VK_F9)&0x8000)!=0;
        if(reloadChord && !lastReloadChord) {
            Emit("LOOT_RELOAD_REQUEST version=1.0.0 trigger=Ctrl+Shift+F9");
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
    if (!D2RL::HasContext(context) || context->apiVersion != D2RL_PLUGIN_API_VERSION)
        return false;
    Context = context;
    if(GetModuleHandleW(L"loot-filter-probe.dll")!=nullptr ||
       GetModuleHandleW(L"d2rl-loot-filter-probe.dll")!=nullptr) {
        context->LogError("LOOT_FILTER_REFUSED legacy loot-filter-probe DLL is also loaded; remove the old plugin before enabling Loot Filter");
        Context=nullptr;
        return false;
    }
    InWorldMode.store(InWorldBackend::Pending, std::memory_order_release);
    InWorldStyleAttached.store(false, std::memory_order_release);
    InWorldStyleCalls.store(0); InWorldStyleWrites.store(0);
    InWorldStyleNoRule.store(0); InWorldStyleUnsupportedColor.store(0);
    InWorldStyleGuarded.store(0);
    InWorldCalls.store(0);
    InWorldDivineCalls.store(0);
    InWorldTextCalls.store(0);
    { std::lock_guard lock(InWorldSampleMutex); LastInWorldSample = {}; }
    InWorldLifecycle = nullptr;
    InWorldJoinedListener = D2RL::Lifecycle::InvalidHandle;
    OriginalInWorldFormatter = nullptr;

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
    SoundAlreadySeen.store(0);SoundObservedAlt.store(0);SoundObservedHidden.store(0);
    SoundPickupPollPending.store(false);
    SoundPickupPolls.store(0);SoundPickupPollSuccess.store(0);
    SoundPickupResets.store(0);SoundPickupBusy.store(0);
    SoundPickupPollRejected.store(0);
    SoundInventory=nullptr;
    SoundInventoryItems=nullptr;
    SoundThreads=nullptr;
    const D2RL::ThreadServiceV1* soundThreads{};
    if(context->QueryService(D2RL::ServiceId::Thread,
           D2RL::ThreadServiceV1Version,&soundThreads)==D2RL::ServiceQueryResult::Success &&
       D2RL::HasThreadServiceV1Field(soundThreads,D2RL::ThreadServiceV1RequiredSize) &&
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
            "LOOT_COMPAT_START version=1.0.0 targetBuild=93847 "
            "sourceMarker=loot-filter-prod-v1 loaderImage=%u peOk=%u stamp=0x%X imageSize=0x%X "
            "threadService=%u quantityPolicy=fail-closed soundPolicy=fail-closed",
            module?1U:0U,peOk?1U:0U,
            peOk?unsigned(nt.FileHeader.TimeDateStamp):0U,
            peOk?unsigned(nt.OptionalHeader.SizeOfImage):0U,
            SoundThreads?1U:0U);
        Emit(line);
        ResetMinimapTracking();
    }

    ActiveGeometryMode.store(GeometryMode::Off, std::memory_order_relaxed);
    BackgroundPaintObserveArmed.store(false,std::memory_order_relaxed);
    BackgroundPaintHookInstalled.store(false,std::memory_order_relaxed);
    OriginalSharedLabelPaint=nullptr;
    GroundIdentities.fill({});
    GroundPropertyLiveReads.store(0,std::memory_order_relaxed);
    GroundPropertyLiveUnknown.store(0,std::memory_order_relaxed);
    GroundIdentityUpdates.store(0,std::memory_order_relaxed);
    GroundIdentitySkips.store(0,std::memory_order_relaxed);
    BackgroundRuleForwarded.store(0,std::memory_order_relaxed);
    BackgroundRuleNoColor.store(0,std::memory_order_relaxed);
    BackgroundRuleNoIdentity.store(0,std::memory_order_relaxed);
    LastDivineTintRecord.store(0,std::memory_order_relaxed);
    LastMapTintRecord.store(0,std::memory_order_relaxed);
    LastDivineTintUnitId.store(0,std::memory_order_relaxed);
    LastMapTintUnitId.store(0,std::memory_order_relaxed);
    BackgroundPaintForwardedPurple.store(0,std::memory_order_relaxed);
    BackgroundPaintIdQualified.store(0,std::memory_order_relaxed);
    BackgroundPaintIdRejected.store(0,std::memory_order_relaxed);
    BackgroundPaintNameRejected.store(0,std::memory_order_relaxed);
    BackgroundPaintColorRejected.store(0,std::memory_order_relaxed);
    BackgroundPaintCalls.store(0,std::memory_order_relaxed);
    BackgroundPaintGroundCalls.store(0,std::memory_order_relaxed);
    BackgroundPaintNeighborCalls.store(0,std::memory_order_relaxed);
    BackgroundPaintDivineCalls.store(0,std::memory_order_relaxed);
    BackgroundPaintDivinePurple.store(0,std::memory_order_relaxed);
    BackgroundPaintDivineBlack.store(0,std::memory_order_relaxed);
    BackgroundPaintMapCalls.store(0,std::memory_order_relaxed);
    BackgroundPaintMapPurple.store(0,std::memory_order_relaxed);
    BackgroundPaintMapBlack.store(0,std::memory_order_relaxed);
    BackgroundPaintOtherCalls.store(0,std::memory_order_relaxed);
    BackgroundPaintReadFailures.store(0,std::memory_order_relaxed);
    BackgroundPaintLockContention.store(0,std::memory_order_relaxed);
    BackgroundPaintLastDivine={};
    BackgroundPaintLastMap={};
    BackgroundPaintLastOther={};
    GroundTextObserveArmed.store(false,std::memory_order_relaxed);
    GroundTextCyanArmed.store(false,std::memory_order_relaxed);
    GroundTextSamples.store(0,std::memory_order_relaxed);
    GroundTextDivine.store(0,std::memory_order_relaxed);
    GroundTextMap.store(0,std::memory_order_relaxed);
    GroundTextReadFailures.store(0,std::memory_order_relaxed);
    GroundTextUnverified.store(0,std::memory_order_relaxed);
    GroundTextCyanWrites.store(0,std::memory_order_relaxed);
    GroundTextCyanRestores.store(0,std::memory_order_relaxed);
    GroundTextCyanAlreadyPresent.store(0,std::memory_order_relaxed);
    GroundTextCyanAfterOriginal.store(0,std::memory_order_relaxed);
    GroundTextNativeAfterOriginal.store(0,std::memory_order_relaxed);
    GroundTextCyanAfterReadFailures.store(0,std::memory_order_relaxed);
    GroundTextCyanColorRejects.store(0,std::memory_order_relaxed);
    GroundTextCyanWriteGuards.store(0,std::memory_order_relaxed);
    GroundTextCyanContended.store(0,std::memory_order_relaxed);
    GroundDivineGlyphSample={};
    GroundMapGlyphSample={};
    BackgroundTintArmed.store(false,std::memory_order_relaxed);
    HideGroundArmed.store(false,std::memory_order_relaxed);
    HiddenGroundPaints.store(0,std::memory_order_relaxed);
    HiddenGroundInteractionPainterSkips.store(0,std::memory_order_relaxed);
    HiddenGroundLastPainterSkipId.store(0,std::memory_order_relaxed);
    HiddenGroundLastPainterSkipCode.store(0,std::memory_order_relaxed);
    HiddenGroundLastPainterSkipMs.store(0,std::memory_order_relaxed);
    HiddenHoverRowsSuppressed.store(0,std::memory_order_relaxed);
    BackgroundTintEverArmed.store(false,std::memory_order_relaxed);
    BackgroundQualified.store(0,std::memory_order_relaxed);
    BackgroundMatched.store(0,std::memory_order_relaxed);
    BackgroundNoMatch.store(0,std::memory_order_relaxed);
    BackgroundGuardFailures.store(0,std::memory_order_relaxed);
    BackgroundWrites.store(0,std::memory_order_relaxed);
    BackgroundRestores.store(0,std::memory_order_relaxed);
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
        // Missing/invalid JSON leaves the filter unarmed; no sample rules
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
        "LOOT_FILTER_READY version=1.0.0 build=%s expectedBuild=93847 config=automatic",
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
    AutomapProjectionArmed.store(false,std::memory_order_release);
    MinimapOverlayRenderer::Shutdown();
    AutomapProjectionHookInstalled.store(false,std::memory_order_release);
    FilterLiveReloadAvailable.store(false,std::memory_order_release);
    NativePickupGuardQualified.store(false,std::memory_order_release);
    GroundQuantityReader.store(nullptr,std::memory_order_release);
    HideGroundArmed.store(false,std::memory_order_release);
    ResetNativeRowFontColor();
    NativeRowBgLiveEnabled.store(false,std::memory_order_release);
    NativeRowBgLiveEpoch.fetch_add(1,std::memory_order_acq_rel);
    InWorldRenderScopeApi.store(nullptr,std::memory_order_release);
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
    DetachInWorldInterop();

    SoundArmed.store(false,std::memory_order_release);
    SoundLoaderBase.store(0,std::memory_order_release);
    ActiveGeometryMode.store(GeometryMode::Off, std::memory_order_release);
    BackgroundTintArmed.store(false,std::memory_order_release);
    BackgroundPaintObserveArmed.store(false,std::memory_order_release);
    GroundTextObserveArmed.store(false,std::memory_order_release);
    GroundTextCyanArmed.store(false,std::memory_order_release);

    std::atomic_store_explicit(&PublishedFilterRules,
        std::shared_ptr<const FilterRuleTable>{},std::memory_order_release);
    FilterConfigPath.clear();
    HookInstalled.store(false, std::memory_order_release);
    OriginalGetItemCode = nullptr;
    // The loader owns detour removal. Do not null out native trampolines
    // while other thread callbacks could still be forwarding through them.

    SoundThreads = nullptr;
    Context = nullptr;
    Base = 0;
    ImageSize = 0;
}

} // namespace SoE::LootFilter
