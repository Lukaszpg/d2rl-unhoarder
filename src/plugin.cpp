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
#include "world_probe_evidence.hpp"
#include "world_probe_label_stack.hpp"
#include "world_probe_upstream_calls.hpp"
#include "hidden_pickup_trace_policy.hpp"
#include "native_action_trace_policy.hpp"
#include "native_pickup_guard_policy.hpp"
#include "filter_rule_engine.hpp"
#include "base_name_table.hpp"
#include "item_type_table.hpp"
#include "ground_property_candidate_compare.hpp"
#include "minimap_world_position_probe_policy.hpp"
#include "automap_projection_probe_policy.hpp"
#include "minimap_overlay_renderer.hpp"
#include "minimap_icon_policy.hpp"
#include "ground_property_live_policy.hpp"
#include "ground_ethereal_policy.hpp"
#include "ground_identified_policy.hpp"
#include "filter_live_reload.hpp"
#include "native_row_live_display_match.hpp"
#include "hover_differential_policy.hpp"
#include "ground_sound_registry.hpp"
#include "named_sound_loader_identity.hpp"
#include "native_row_string_layout.hpp"
#include "native_row_label_correlation.hpp"
#include "native_row_handoff_scan.hpp"
#include "native_row_append_match.hpp"
#include "native_row_latest_match.hpp"
#include "native_row_bg_trial_policy.hpp"
#include "native_row_bg_live_policy.hpp"
#include "native_row_font_color_policy.hpp"
#include <D2RLPlugin/shared_events.h>
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
constexpr std::uintptr_t InvestigatedCallerRva = 0x662A3D;
constexpr std::array<std::uint8_t, 32> ExpectedGetItemCode{
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
    0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48, 0x85, 0xC9,
    0x75, 0x13, 0x88, 0x4C, 0x24, 0x30, 0x48, 0x8D,
    0x4C, 0x24, 0x30, 0xE8, 0x80, 0x83, 0xFF, 0xFF,
};
constexpr std::array<std::uintptr_t, 7> HistoricalLabelLimitAnchors{
    0x1516EC1, 0x1516ED1, 0x1516F43,
    0x1519A1B, 0x1519A52, 0x1519AAD, 0x1519AFE
};
// Live 93847 helper called by the historical label-record path at 0x1516ECC.
// This is an observed collection helper, NOT a verified item-label renderer.
constexpr std::uintptr_t CollectionHelperRva = 0x1517C70;
constexpr std::uintptr_t CollectionCallRva = 0x1516ECC;
constexpr std::array<std::uint8_t, 16> ExpectedCollectionHelper{
    0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20
};
constexpr std::array<std::uint8_t, 5> ExpectedCollectionCaller{
    0xE8, 0x9F, 0x0D, 0x00, 0x00
};
constexpr std::uint32_t DivineCode =
    static_cast<std::uint32_t>('d') |
    (static_cast<std::uint32_t>('i') << 8U) |
    (static_cast<std::uint32_t>('v') << 16U) |
    (static_cast<std::uint32_t>('o') << 24U);
constexpr std::size_t MaximumPhases = 12;
constexpr std::size_t MaximumRows = 512;
constexpr std::size_t MaximumDumpRows = 80;
constexpr ULONGLONG CaptureMilliseconds = 8'000;
// A candidate display record: the suite patch metadata suggests a 0x144-byte
// stride.  We copy at most this exact region, without following pointers.
constexpr std::size_t RecordBytes = 0x144;
constexpr std::size_t CandidateTextOffset = 0x28; // relative to arg3, unverified
constexpr std::size_t CandidateTextMaximum = 96;
constexpr std::size_t CandidateTextVariants = 16;
constexpr std::uint64_t TextSamplingInterval = 7;
// The known container is capped at 32 and prior patch metadata identifies a
// 0x144-byte stride. Treat all 32 slots as *candidate* records, not a proven
// item count. Only enumerate while an explicit collection capture is armed.
// Formatter 0x1FA9F0 has SIX arguments: the 5th and 6th are stack-
// passed by 0x15171E5. Never replace it with a four-argument thunk.
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
constexpr char LongDivineLabel[] = "FILTERED DIVINE ORB - LONG LABEL TEST";
constexpr char ShortDivineLabel[] = "ORB";
static_assert(sizeof(LongDivineLabel) <= CandidateTextMaximum);
static_assert(sizeof(ShortDivineLabel) <= CandidateTextMaximum);
static_assert(sizeof(LongDivineLabel) <= InnerNameBufferBytes - InnerNamePrefixBytes);
// 0 disabled, 1 observe only, 2 long name, 3 short name.
enum class GeometryMode : std::uint32_t { Off, Observe, Long, Short, Rules };
constexpr std::size_t FormatterSlotsPerPhase = 8;
// First, explicitly opt-in, same-length GROUND LABEL ONLY PoC.
// classId=0x2C2 is observed for the Divine Orb in the 0.1.16 build 93847
// capture. This is NOT a general item-code resolver, nor a portable ID.
constexpr std::uint32_t DivineNativeClassId = 0x2C2;
constexpr char OriginalDivineLabel[] = "Divine Orb";
constexpr char ReplacementDivineLabel[] = "FILTER ORB";
static_assert(sizeof(OriginalDivineLabel) == sizeof(ReplacementDivineLabel));
constexpr std::size_t CandidateSlotCount = 32;
constexpr std::uint64_t SlotSamplingInterval = 79;

using GetItemCodeFn = std::uint32_t(__fastcall*)(void*) noexcept;
// The x64 ABI returns a scalar in RAX, if any. We forward all four register
// arguments and the original RAX to avoid changing the caller's observable state.
// Do not infer semantic types or dereference any argument in the observer.
using CollectionHelperFn = std::uint64_t(__fastcall*)(
    void*, void*, void*, std::uint64_t) noexcept;
using LabelFormatterFn = std::uint8_t(__fastcall*)(
    void*, void*, void*, std::uint32_t, std::uint64_t, std::uint64_t) noexcept;
LabelFormatterFn OriginalLabelFormatter{};
std::atomic_bool FormatterHookInstalled{false};
std::atomic<std::uint64_t> FormatterCalls{};
std::atomic<std::uint64_t> FormatterContention{};
// Disabled by default. The switch is explicit, reversible and game-session-only.
std::atomic_bool RenameArmed{};
std::atomic<std::uint64_t> RenameQualified{};
std::atomic<std::uint64_t> RenameNonDivine{};
std::atomic<std::uint64_t> RenameIdMismatch{};
std::atomic<std::uint64_t> RenameTextMismatch{};
std::atomic<std::uint64_t> RenameReadFailures{};
std::atomic<std::uint64_t> RenameWrites{};
// New opt-in code-matched label PoC: no Divine class ID or original name
// comparison. Rule lookup uses the formatter unit and the proven helper
// trampoline. Fixed-width substitution is intentional until geometry is solved.
struct CodeNameRule {
    std::uint32_t code;
    const char* replacement;
    std::size_t replacementBytes; // INCLUDING the trailing NUL
};
constexpr std::array<CodeNameRule, 1> CodeNameRules{{
    {DivineCode, ReplacementDivineLabel, sizeof(ReplacementDivineLabel)},
}};
// Separate opt-in pre-measurement name writer hook, never co-armed with
// the legacy after-measurement experiments. No manual geometry arithmetic.
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

// Declared before the ground-text status reporter; defined after capture setup.
void Emit(const char* message) noexcept;
// Declared before 0.2.39 ground evidence drainers; defined with other text helpers.
void CodeText(std::uint32_t code, char (&out)[5]) noexcept;

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



std::atomic_bool CodeRenameArmed{};
std::atomic<std::uint64_t> CodeRenameQualified{};
std::atomic<std::uint64_t> CodeRenameIdMismatch{};
std::atomic<std::uint64_t> CodeRenameLookups{};
std::atomic<std::uint64_t> CodeRenameInvalid{};
std::atomic<std::uint64_t> CodeRenameNoRule{};
std::atomic<std::uint64_t> CodeRenameRuleMatches{};
std::atomic<std::uint64_t> CodeRenameTextLengthMismatch{};
std::atomic<std::uint64_t> CodeRenameReadFailures{};
std::atomic<std::uint64_t> CodeRenameWrites{};
// Opt-in code bridge: sample the *formatter native unit* using the game's
// existing item-code helper trampoline. This is intentionally NOT enabled by
// rename-arm and does not change the existing class-ID rename behavior.
// Only one helper call per newly claimed (unit, record) observation, during an
// explicit timed capture. Do not start calling this from every frame.
std::atomic_bool CodeBridgeArmed{};
std::atomic<std::uint64_t> CodeBridgeAttempts{};
std::atomic<std::uint64_t> CodeBridgeSuccess{};
std::atomic<std::uint64_t> CodeBridgeGuardReject{};
std::atomic<std::uint64_t> CodeBridgeInvalid{};
std::atomic<std::uint64_t> CodeBridgeDivine{};
// The map was observed with class ID 0x2BA; its base code has NOT been
// established. In particular, mp04 appears even with no map on the ground.
constexpr std::uint32_t ObservedMapNativeClassId = 0x2BA;
std::atomic<std::uint64_t> CodeBridgeMap{};
std::atomic<std::uint64_t> CodeBridgeOther{};
std::atomic<std::uint64_t> CodeBridgeDivineMismatch{};
const D2RL::PluginContext* Context{};
std::uintptr_t Base{};
std::uint32_t ImageSize{};
GetItemCodeFn OriginalGetItemCode{};
std::atomic_bool HookInstalled{false};

// Build 93847 / D2RLoader 1.3: native ItemStatCost `quantity` is stat 70.
// Use the loader-owned GetUnitStat bridge, NEVER an unqualified direct
// D2RCore call or another hook. This is initialized once after SoE attaches.
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



// Socket qualification observer retained from v0.2.50. Native stat 194 was
// confirmed against SDK for ground mode-3 counts 0..6. The active v0.2.51
// reader below also admits mode 5 ONLY through verified label presentation.
// F8 observer remains opt-in, read-only, with no native item writes.
constexpr std::int32_t SocketProbeCandidateStatId=194;
constexpr std::size_t SocketProbeMaxItems=12;
constexpr std::size_t SocketProbeMaxSamples=SocketProbeMaxItems*2;
struct SocketProbeRow {
    std::uint32_t code{},classId{},unitId{},mode{};
    bool candidateKnown{},sdkMatched{},reported{},sdkReported{};
    std::int32_t candidate{};
    std::uint32_t sdkSockets{},sdkQuality{},sdkContainer{};
};
std::atomic_bool SocketProbeArmed{};
std::mutex SocketProbeMutex;
std::array<SocketProbeRow,SocketProbeMaxSamples> SocketProbeRows{};
std::size_t SocketProbeCount{};
std::atomic<std::uint32_t> SocketProbeContention{};

void ArmSocketProbe() noexcept {
    SocketProbeArmed.store(false,std::memory_order_release);
    {
        std::lock_guard lock(SocketProbeMutex);
        SocketProbeRows.fill({});SocketProbeCount=0;
    }
    SocketProbeContention.store(0,std::memory_order_relaxed);
    SocketProbeArmed.store(true,std::memory_order_release);
    Emit("LOOT_SOCKET_PROBE_ARMED version=1.0.0 trigger=Ctrl+Shift+F8 "
         "source=verified-ground-label-mode5-or-mode3 "
         "candidate=item_numsockets-stat194 via qualified-GetUnitStat "
         "sdk=post-pickup-runtime-id+code+classId "
         "maxItems=12 maxModesPerItem=2 zeroIsCandidateNotVerified=1 "
         "socketsJson=SUPPORTED activeRulesReadSeparate=1 pickupUnchanged=1");
}

// Called only from an existing verified label or native formatter record
// path. The label owner provides exact code, expected runtime ID/classId.
// Validate both UnitAny headers around the qualified stat call: do NOT treat
// a transient mode-5 unit as a mode-3 pickup candidate.
void ObserveSocketProbe(const void* unit,std::uint32_t code,
    std::uint32_t id,std::uint32_t classId) noexcept {
    if(!SocketProbeArmed.load(std::memory_order_acquire) || !unit || !id ||
       !code || !Context || !D2RL::GetBuildName(Context) ||
       std::string_view(D2RL::GetBuildName(Context))!="93847") return;
    code=CanonicalItemCode(code);
    std::array<std::uint32_t,4> before{},after{};
    SIZE_T copied{};
    if(!ReadProcessMemory(GetCurrentProcess(),unit,before.data(),
           sizeof(before),&copied) || copied!=sizeof(before) ||
       before[0]!=4 || before[1]!=classId || before[2]!=id ||
       !GroundPropertyLive::AllowsMode(before[3],
           GroundPropertyLive::Purpose::VerifiedLabel)) return;
    // Fast duplicate/identity cap before any native getter call.
    if(!SocketProbeMutex.try_lock()) {
        SocketProbeContention.fetch_add(1,std::memory_order_relaxed);return;
    }
    bool duplicate=false;
    std::size_t identities=0;
    for(std::size_t i=0;i<SocketProbeCount;++i) {
        const auto& row=SocketProbeRows[i];
        if(row.unitId==id && row.code==code && row.classId==classId) {
            if(row.mode==before[3]) duplicate=true;
        } else {
            bool first=true;
            for(std::size_t j=0;j<i;++j)
                if(SocketProbeRows[j].unitId==row.unitId &&
                   SocketProbeRows[j].code==row.code &&
                   SocketProbeRows[j].classId==row.classId) {first=false;break;}
            if(first) ++identities;
        }
    }
    const bool alreadyPresent=std::any_of(SocketProbeRows.begin(),
        SocketProbeRows.begin()+SocketProbeCount,
        [=](const SocketProbeRow& row) {
            return row.unitId==id && row.code==code && row.classId==classId;
        });
    const bool capacityFull=SocketProbeCount>=SocketProbeMaxSamples ||
        (!alreadyPresent && identities>=SocketProbeMaxItems);
    SocketProbeMutex.unlock();
    if(duplicate || capacityFull) return;
    const auto getter=GroundQuantityReader.load(std::memory_order_acquire);
    bool candidateKnown=false;
    std::int32_t candidate{};
    if(getter) {
        const auto value=getter(const_cast<void*>(unit),
            SocketProbeCandidateStatId,0);
        // Candidate range only, not a qualification. Known zero differs from
        // absent bridge and should be checked against SDK's zero socketCount.
        if(value>=0 && value<=15) {
            candidate=value;candidateKnown=true;
        }
    }
    copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),unit,after.data(),
           sizeof(after),&copied) || copied!=sizeof(after) ||
       before!=after) return;
    if(!SocketProbeMutex.try_lock()) {
        SocketProbeContention.fetch_add(1,std::memory_order_relaxed);return;
    }
    duplicate=false;identities=0;bool alreadyPresentNow=false;
    for(std::size_t i=0;i<SocketProbeCount;++i) {
        const auto& row=SocketProbeRows[i];
        if(row.unitId==id && row.code==code && row.classId==classId) {
            alreadyPresentNow=true;
            if(row.mode==before[3]) duplicate=true;
        }
        bool first=true;
        for(std::size_t j=0;j<i;++j)
            if(SocketProbeRows[j].unitId==row.unitId &&
               SocketProbeRows[j].code==row.code &&
               SocketProbeRows[j].classId==row.classId) {first=false;break;}
        if(first) ++identities;
    }
    if(!duplicate && SocketProbeCount<SocketProbeMaxSamples &&
       (alreadyPresentNow || identities<SocketProbeMaxItems)) {
        auto& row=SocketProbeRows[SocketProbeCount++];
        row.code=code;row.classId=classId;row.unitId=id;
        row.mode=before[3];row.candidateKnown=candidateKnown;
        row.candidate=candidate;
    }
    SocketProbeMutex.unlock();
}

void ObserveSocketProbeCarriedSdk(const D2RL::Items::ItemInfo* info) noexcept {
    if(!info || info->structSize<D2RL::Items::ItemInfoRequiredSize ||
       !info->runtimeId ||
       info->container==D2RL::Items::ItemContainer::Ground ||
       !SocketProbeMutex.try_lock()) return;
    for(std::size_t i=0;i<SocketProbeCount;++i) {
        auto& row=SocketProbeRows[i];
        if(row.unitId==info->runtimeId &&
           row.code==CanonicalItemCode(info->code) &&
           row.classId==info->classId && !row.sdkMatched) {
            row.sdkMatched=true;row.sdkSockets=info->socketCount;
            row.sdkQuality=static_cast<std::uint32_t>(info->quality);
            row.sdkContainer=static_cast<std::uint32_t>(info->container);
        }
    }
    SocketProbeMutex.unlock();
}

void DrainSocketProbe() noexcept {
    std::array<SocketProbeRow,SocketProbeMaxSamples> pending{};
    std::size_t count{};
    if(!SocketProbeMutex.try_lock())return;
    for(std::size_t i=0;i<SocketProbeCount;++i) {
        auto& row=SocketProbeRows[i];
        if(!row.reported || (row.sdkMatched && !row.sdkReported)) {
            pending[count++]=row;
            row.reported=true;
            if(row.sdkMatched) row.sdkReported=true;
        }
    }
    SocketProbeMutex.unlock();
    for(std::size_t i=0;i<count;++i) {
        const auto& row=pending[i];
        char code[5]{};CodeText(row.code,code);
        char line[400]{};
        if(row.sdkMatched) {
            std::snprintf(line,sizeof(line),
                "LOOT_SOCKET_PROBE_SDK_COMPARE version=1.0.0 "
                "code='%.4s' unitId=%u classId=%u mode=%u "
                "stat194=%d candidateKnown=%u sdkSockets=%u sdkQuality=%u "
                "sdkContainer=%u result=%s postPickup=1 "
                "mode5NeedsIndependentVerification=1 socketsJson=SUPPORTED",
                code,row.unitId,row.classId,row.mode,row.candidate,
                row.candidateKnown?1U:0U,row.sdkSockets,row.sdkQuality,
                row.sdkContainer,!row.candidateKnown?"UNAVAILABLE":
                    row.candidate==static_cast<std::int32_t>(row.sdkSockets)?
                    "MATCH":"MISMATCH");
        } else {
            std::snprintf(line,sizeof(line),
                "LOOT_SOCKET_PROBE_SAMPLE version=1.0.0 "
                "code='%.4s' unitId=%u classId=%u mode=%u "
                "stat194=%d candidateKnown=%u sdk=AWAITING_PICKUP "
                "zeroIsCandidateNotVerified=1 socketsJson=SUPPORTED",
                code,row.unitId,row.classId,row.mode,row.candidate,
                row.candidateKnown?1U:0U);
        }
        Emit(line);
    }
    const auto contention=SocketProbeContention.exchange(0,
        std::memory_order_relaxed);
    if(contention) {
        char line[140]{};
        std::snprintf(line,sizeof(line),
            "LOOT_SOCKET_PROBE_CONTENTION version=1.0.0 count=%u "
            "samples-may-be-incomplete=1",contention);
        Emit(line);
    }
}

// Read only fields already verified by the 0.2.39 formatter/pickup paths:
// UnitAny type at +0, classId at +4, id at +8; stat70 from an admitted
// loader-owned reader. No unqualified ItemData/quality/ilvl/flag offsets.
// Definitions appear later; the opt-in evidence probe only consumes known data.
bool PrintableItemCode(std::uint32_t code) noexcept;

// 0.2.39: on-demand, read-only ground item/property correlation evidence.
// Native UnitAny +0x10 is a candidate pointer from 0.2.36 capture, NOT a
// qualified ItemData pointer. One guarded hop ONLY during F8 evidence. No
// pointer-like fields found in the target are ever chased. No rule changes.
// The *same qualified native pointer* is used by label and pickup paths.
// Do not interpret these bytes as ItemData/quality/level/flags until correlated
// against known items and qualified native readers for build 93847.
// Ctrl+Shift+F8 captures at most 12 distinct (code,id) ground formatter units;
// captures have no expiration and never participate in visibility decisions.
constexpr std::size_t GroundPropertyEvidenceCapacity=12;
constexpr std::size_t GroundPropertyEvidenceBytes=0xC0;
constexpr std::size_t GroundPropertyEvidenceFallbackBytes=0x80;
constexpr std::size_t GroundPropertyCandidateBytes=0x100;
// Do not encode guessed field meanings here. See the portable probe policy.
enum class GroundPropertyCandidateStatus : std::uint32_t {
    Unchecked=0, ZeroOrNoncanonical=1, BadPage=2, ReadFailed=3,
    ReadOk=4, IdentityChanged=5
};
struct GroundPropertySdkEvidence {
    bool matched{};
    GroundCandidateProbe::Snapshot candidateSnapshot{};
    std::uint32_t code{},runtimeId{},classId{},quality{},itemLevel{};
    std::uint32_t sockets{},stateFlags{},container{};
};
struct GroundPropertyEvidence {
    std::uint32_t code{},classId{},unitId{},mode{};
    std::uint32_t bytesRead{};
    std::array<std::uint8_t,GroundPropertyEvidenceBytes> bytes{};
    // A scalar address is retained only within this bounded diagnostic queue.
    // It is never logged, used for identity lookup, or consulted by rule eval.
    std::uintptr_t candidateAddress{};
    GroundPropertyCandidateStatus candidateStatus{
        GroundPropertyCandidateStatus::Unchecked};
    std::uint32_t candidateBytesRead{};
    std::array<std::uint8_t,GroundPropertyCandidateBytes> candidateBytes{};
    GroundPropertySdkEvidence sdk{};
};
std::atomic_bool GroundPropertyEvidenceArmed{};
std::mutex GroundPropertyEvidenceMutex;
std::array<GroundPropertyEvidence,GroundPropertyEvidenceCapacity>
    GroundPropertyEvidenceRows{};
std::size_t GroundPropertyEvidenceCount{},GroundPropertyEvidenceReported{};
std::atomic<std::uint64_t> GroundPropertyEvidenceReadFailed{};
std::atomic<std::uint64_t> GroundPropertyEvidenceContended{};

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

// v1.0.0 — qualified ground position feeding JSON-configured automap markers.
// Based on SoE 0.18.244 native_contract.hpp portal-coordinate witness:
//   UnitAny.pStaticPath +0x38; D2StaticPath.tGameCoord +0x10.
// v0.2.63 qualified this layout for ground items on build 93847: the same
// Divine Orb changed 5110,5015 -> 5072,5154 after pickup/move/redrop, with
// exact mode-5/mode-3 agreement at both locations. No game-facing code writes
// through this path; it remains a guarded read-only ground-position reader.
constexpr std::size_t MinimapProbeCapacity=32;
constexpr std::size_t MinimapUnitBytes=0x40;
constexpr std::size_t MinimapPathBytes=0x28;
enum class MinimapProbeStatus : std::uint8_t {
    UnitReadFailed, NullPath, PathUnreadable,
    PathChanged, ImplausibleCoords, CandidateCoord
};
struct MinimapProbeSample {
    std::uint32_t code{},unitId{},classId{},mode{};
    std::uint32_t x{},y{};
    MinimapProbeStatus status{MinimapProbeStatus::UnitReadFailed};
    std::array<std::uint8_t,16> coordWindow{}; // bytes at staticPath+0x10..0x1F
};
std::atomic_bool MinimapProbeArmed{};
std::mutex MinimapProbeMutex;
std::array<MinimapProbeSample,MinimapProbeCapacity> MinimapProbeRows{};
std::size_t MinimapProbeCount{},MinimapProbeReported{};
std::atomic<std::uint64_t> MinimapProbeContended{};

// v1.0.0 — standalone native automap projection feeding a copied marker frame.
// MapSense 2.0.2 independently qualified these build-93847 native contracts:
//   D2R+0xD76E0 RenderAutomapUnit(unit, AutomapContext*)
//   D2R+0xD4910 ProjectClientToAutomap(context, out, packedClientXY)
// The loot filter ports only the small projection contract; MapSense is NOT a
// runtime dependency. The hook forwards D2R exactly once and records bounded
// diagnostic samples only after the local-player automap pass is proven.
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
constexpr std::uint64_t MinimapProjectionSampleIntervalMilliseconds=100;
constexpr std::size_t MinimapProjectionItemCapacity=256;
constexpr std::size_t MinimapProjectionSampleCapacity=96;

struct NativeAutomapPoint final {
    std::int32_t x{};
    std::int32_t y{};
};
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
std::atomic<std::uint64_t> AutomapProjectionPasses{};
std::atomic<std::uint64_t> AutomapProjectionProjectCalls{};
std::atomic<std::uint64_t> AutomapProjectionFailures{};
std::atomic<std::uint64_t> AutomapProjectionContention{};
std::atomic<std::uint64_t> AutomapProjectionLastSampleTick{};
std::atomic<std::uint64_t> AutomapMarkerFrameSequence{};
std::atomic_bool AutomapMarkerFirstPublishLogged{};

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
std::array<MinimapProjectionItem,MinimapProjectionItemCapacity>
    MinimapProjectionItems{};
std::size_t MinimapProjectionItemCount{};

struct MinimapProjectionSample final {
    std::uint32_t code{},unitId{},classId{},mode{};
    std::int32_t worldX{},worldY{};
    AutomapProjectionProbe::Point client{};
    AutomapProjectionProbe::Point screen{};
    AutomapProjectionProbe::ClipRect clip{};
    std::uint64_t itemAgeMs{};
    std::uint32_t threadId{};
    bool visible{};
};
std::mutex AutomapProjectionSampleMutex;
std::array<MinimapProjectionSample,MinimapProjectionSampleCapacity>
    AutomapProjectionSamples{};
std::size_t AutomapProjectionSampleCount{},AutomapProjectionSampleReported{};

void ResetAutomapProjectionProbe() noexcept {
    AutomapProjectionArmed.store(false,std::memory_order_release);
    {
        std::lock_guard lock(MinimapProjectionItemMutex);
        MinimapProjectionItems.fill({});
        MinimapProjectionItemCount=0;
    }
    {
        std::lock_guard lock(AutomapProjectionSampleMutex);
        AutomapProjectionSamples.fill({});
        AutomapProjectionSampleCount=0;
        AutomapProjectionSampleReported=0;
    }
    AutomapProjectionPasses.store(0,std::memory_order_relaxed);
    AutomapProjectionProjectCalls.store(0,std::memory_order_relaxed);
    AutomapProjectionFailures.store(0,std::memory_order_relaxed);
    AutomapProjectionContention.store(0,std::memory_order_relaxed);
    AutomapProjectionLastSampleTick.store(0,std::memory_order_relaxed);
    AutomapMarkerFrameSequence.store(0,std::memory_order_relaxed);
    AutomapMarkerFirstPublishLogged.store(false,std::memory_order_relaxed);
    MinimapOverlayRenderer::Clear();
    AutomapProjectionArmed.store(true,std::memory_order_release);
}

void UpdateMinimapProjectionItem(const MinimapProbeSample& sample) noexcept {
    if(sample.status!=MinimapProbeStatus::CandidateCoord || sample.unitId==0) return;
    if(!MinimapProjectionItemMutex.try_lock()) {
        AutomapProjectionContention.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    const auto now=static_cast<std::uint64_t>(GetTickCount64());
    std::size_t slot=MinimapProjectionItemCount;
    for(std::size_t i=0;i<MinimapProjectionItemCount;++i) {
        if(MinimapProjectionItems[i].unitId==sample.unitId &&
           MinimapProjectionItems[i].code==sample.code) {
            slot=i;break;
        }
    }
    if(slot==MinimapProjectionItemCount) {
        if(MinimapProjectionItemCount<MinimapProjectionItemCapacity) {
            slot=MinimapProjectionItemCount++;
            MinimapProjectionItems[slot]={};
        } else {
            slot=0;
            for(std::size_t i=1;i<MinimapProjectionItemCount;++i) {
                if(MinimapProjectionItems[i].observedTick<
                   MinimapProjectionItems[slot].observedTick) slot=i;
            }
            MinimapProjectionItems[slot]={};
        }
    }
    auto& item=MinimapProjectionItems[slot];
    item.code=sample.code;item.unitId=sample.unitId;
    item.classId=sample.classId;item.mode=sample.mode;
    item.worldX=static_cast<std::int32_t>(sample.x);
    item.worldY=static_cast<std::int32_t>(sample.y);
    item.observedTick=now;
    MinimapProjectionItemMutex.unlock();
}

void UpdateMinimapProjectionIconRule(std::uint32_t unitId,std::uint32_t code,
    const GroundRuleDecision* rule) noexcept {
    if(unitId==0) return;
    if(!MinimapProjectionItemMutex.try_lock()) {
        AutomapProjectionContention.fetch_add(1,std::memory_order_relaxed);
        return;
    }
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
            for(std::size_t i=1;i<MinimapProjectionItemCount;++i) {
                if(MinimapProjectionItems[i].observedTick<
                   MinimapProjectionItems[slot].observedTick) slot=i;
            }
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
    if(unitId==0) return;
    if(!MinimapProjectionItemMutex.try_lock()) {
        AutomapProjectionContention.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    bool removed{};
    for(std::size_t i=0;i<MinimapProjectionItemCount;) {
        if(MinimapProjectionItems[i].unitId!=unitId) {
            ++i;
            continue;
        }
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
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        AutomapProjectionFailures.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
}

void RecordAutomapProjectionSample(const MinimapProjectionSample& sample) noexcept {
    if(!AutomapProjectionSampleMutex.try_lock()) {
        AutomapProjectionContention.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    const bool duplicate=std::any_of(AutomapProjectionSamples.begin(),
        AutomapProjectionSamples.begin()+AutomapProjectionSampleCount,
        [&](const MinimapProjectionSample& old) {
            return old.unitId==sample.unitId && old.worldX==sample.worldX &&
                old.worldY==sample.worldY && old.screen.x==sample.screen.x &&
                old.screen.y==sample.screen.y && old.clip.left==sample.clip.left &&
                old.clip.top==sample.clip.top && old.clip.width==sample.clip.width &&
                old.clip.height==sample.clip.height && old.visible==sample.visible;
        });
    if(!duplicate && AutomapProjectionSampleCount<MinimapProjectionSampleCapacity) {
        AutomapProjectionSamples[AutomapProjectionSampleCount++]=sample;
    }
    AutomapProjectionSampleMutex.unlock();
}

__declspec(noinline) void __fastcall HookAutomapRenderUnit(
        void* unit,void* automapContext) noexcept {
    const auto original=OriginalAutomapRenderUnit;
    if(!original)return;
    original(unit,automapContext);
    if(!AutomapProjectionArmed.load(std::memory_order_acquire) ||
       !automapContext || !IsLocalPlayerAutomapPass(unit)) return;
    AutomapProjectionPasses.fetch_add(1,std::memory_order_relaxed);

    const auto now=static_cast<std::uint64_t>(GetTickCount64());
    std::array<MinimapProjectionItem,MinimapProjectionItemCapacity> items{};
    std::size_t itemCount{};
    if(!MinimapProjectionItemMutex.try_lock()) {
        AutomapProjectionContention.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    for(std::size_t i=0;i<MinimapProjectionItemCount;++i) {
        const auto& item=MinimapProjectionItems[i];
        if(!item.hasIcon || item.observedTick==0 || now<item.observedTick ||
           now-item.observedTick>MinimapProjectionItemFreshMilliseconds) continue;
        items[itemCount++]=item;
    }
    MinimapProjectionItemMutex.unlock();
    if(!itemCount) {
        MinimapOverlayRenderer::Clear();
        return;
    }

    __try {
        const auto* contextBytes=static_cast<const std::uint8_t*>(automapContext);
        const AutomapProjectionProbe::ClipRect clip{
            *reinterpret_cast<const std::int32_t*>(contextBytes+AutomapClipLeftOffset),
            *reinterpret_cast<const std::int32_t*>(contextBytes+AutomapClipTopOffset),
            *reinterpret_cast<const std::int32_t*>(contextBytes+AutomapClipWidthOffset),
            *reinterpret_cast<const std::int32_t*>(contextBytes+AutomapClipHeightOffset)};
        if(!AutomapProjectionProbe::PlausibleClip(clip) || !ProjectClientToAutomap) {
            AutomapProjectionFailures.fetch_add(1,std::memory_order_relaxed);
            MinimapOverlayRenderer::Clear();
            return;
        }

        MinimapOverlayRenderer::MarkerFrame markerFrame{};
        markerFrame.clip={
            static_cast<float>(clip.left),
            static_cast<float>(clip.top),
            static_cast<float>(static_cast<std::int64_t>(clip.left)+clip.width),
            static_cast<float>(static_cast<std::int64_t>(clip.top)+clip.height)};
        markerFrame.publishedTick=now;
        markerFrame.sequence=AutomapMarkerFrameSequence.fetch_add(
            1,std::memory_order_relaxed)+1;

        for(std::size_t i=0;i<itemCount;++i) {
            const auto& item=items[i];
            AutomapProjectionProbe::Point client{};
            if(!AutomapProjectionProbe::WorldSubtileToClient(
                    item.worldX,item.worldY,client)) {
                AutomapProjectionFailures.fetch_add(1,std::memory_order_relaxed);
                continue;
            }
            NativeAutomapPoint projected{};
            AutomapProjectionProjectCalls.fetch_add(1,std::memory_order_relaxed);
            if(ProjectClientToAutomap(automapContext,&projected,
                    AutomapProjectionProbe::PackClientCoordinates(client))!=&projected) {
                AutomapProjectionFailures.fetch_add(1,std::memory_order_relaxed);
                continue;
            }
            const AutomapProjectionProbe::Point screen{projected.x,projected.y};
            const bool visible=AutomapProjectionProbe::Contains(clip,screen);
            if(visible && markerFrame.count<markerFrame.markers.size()) {
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

        if(markerFrame.count) {
            MinimapOverlayRenderer::Publish(markerFrame);
        } else {
            MinimapOverlayRenderer::Clear();
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        AutomapProjectionFailures.fetch_add(1,std::memory_order_relaxed);
        MinimapOverlayRenderer::Clear();
    }
}

bool InstallStandaloneAutomapProjectionObserver() noexcept {
    AutomapProjectionHookInstalled.store(false,std::memory_order_release);
    OriginalAutomapRenderUnit=nullptr;
    ProjectClientToAutomap=nullptr;
    GetLocalDataContext=nullptr;
    GetLocalPlayer=nullptr;
    if(!Context || !Base || !ImageSize) {
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=1.0.0 reason=no-image-or-context hooks=0 mapSenseDependency=0");
        return false;
    }
    // MapSense 2.x owns this same native rendezvous when loaded. Until it
    // exposes a projection service, preserve coexistence by refusing our
    // standalone hook rather than displacing or blind-chaining its owner.
    if(GetModuleHandleW(L"d2rl-ruffneckk-mapsense.dll")!=nullptr) {
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=1.0.0 reason=mapsense-loaded-shared-rendezvous hooks=0 mapSenseDependency=0 coexistence=preserved projectionService=not-yet-exposed");
        return false;
    }
    const auto check=[&](std::uintptr_t rva,const auto& expected) {
        return Context->CheckExpectedBytes(rva,expected.data(),
            static_cast<std::uint32_t>(expected.size()));
    };
    if(!check(ProjectClientToAutomapRva,ExpectedProjectClientToAutomap) ||
       !check(GetLocalDataContextRva,ExpectedGetLocalDataContext) ||
       !check(GetLocalPlayerRva,ExpectedGetLocalPlayer)) {
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=1.0.0 reason=dependency-fingerprint hooks=0 mapSenseDependency=0");
        return false;
    }
    if(!check(AutomapRenderUnitRva,ExpectedAutomapRenderUnit)) {
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=1.0.0 reason=render-entry-fingerprint-or-foreign-owner hooks=0 mapSenseDependency=0 noBlindChain=1");
        return false;
    }
    ProjectClientToAutomap=reinterpret_cast<ProjectClientToAutomapFn>(
        Base+ProjectClientToAutomapRva);
    GetLocalDataContext=reinterpret_cast<GetLocalDataContextFn>(
        Base+GetLocalDataContextRva);
    GetLocalPlayer=reinterpret_cast<GetLocalPlayerFn>(Base+GetLocalPlayerRva);
    if(!Context->InstallInlineHook(AutomapRenderUnitRva,
            ExpectedAutomapRenderUnit.data(),
            static_cast<std::uint32_t>(ExpectedAutomapRenderUnit.size()),
            HookAutomapRenderUnit,&OriginalAutomapRenderUnit) ||
       !OriginalAutomapRenderUnit) {
        ProjectClientToAutomap=nullptr;
        GetLocalDataContext=nullptr;
        GetLocalPlayer=nullptr;
        Emit("LOOT_MINIMAP_PROJECTION_REFUSED version=1.0.0 reason=loader-hook-registration hooks=0 mapSenseDependency=0");
        return false;
    }
    AutomapProjectionHookInstalled.store(true,std::memory_order_release);
    Emit("LOOT_MINIMAP_PROJECTION_READY version=1.0.0 hook=D2R+0xD76E0 "
         "project=D2R+0xD4910 localContext=D2R+0x8B2D0 localPlayer=D2R+0x9A480 "
         "abi=RenderAutomapUnit(unit,context)+ProjectClientToAutomap(context,out,packedXY) "
         "worldToClient='x=16*(worldX-worldY),y=8*(worldX+worldY)' "
         "clip=context+0x18,+0x1C,+0x20,+0x24 target=json-minimapIcon-rules "
         "mapSenseDependency=0 standalone=1 coexistence=fail-closed-if-mapsense-loaded "
         "originalForwardedOnce=1 drawing=renderer-separate gameplayWrites=0");
    return true;
}

void __cdecl LogMinimapRendererDiagnostic(const char* message) noexcept {
    Emit(message);
}

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
        Emit("LOOT_MINIMAP_AUTOMAP_GATE_REFUSED version=1.0.0 reason=ui-state-fingerprint drawing=0 no-timeout-only-fallback=1");
        return false;
    }
    MinimapOverlayRenderer::SetAutomapVisibilityTable(
        reinterpret_cast<const volatile std::uint8_t*>(
            Base+NativeUiStateTableRva));
    Emit("LOOT_MINIMAP_AUTOMAP_GATE_READY version=1.0.0 "
         "table=D2R+0x2A2ADA0 automapState=10 witnesses=0xCD7FB,0xC7DF1,0xCDE3C "
         "readOnly=1 closeSuppression=immediate-present-frame staleTimeoutMs=250");
    return true;
}

bool InitializeMinimapMarkerRenderer() noexcept {
    HMODULE self{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&InitializeMinimapMarkerRenderer),&self) || !self) {
        Emit("LOOT_MINIMAP_RENDERER_REFUSED version=1.0.0 backend=none reason=self-module-unresolved projectionContinues=1 drawing=0");
        return false;
    }
    if(!ConfigureNativeAutomapVisibilityGate()) {
        Emit("LOOT_MINIMAP_RENDERER_REFUSED version=1.0.0 backend=none reason=automap-visibility-gate-unqualified projectionContinues=1 drawing=0");
        return false;
    }
    MinimapOverlayRenderer::SetDllModule(self);
    MinimapOverlayRenderer::SetLogCallback(&LogMinimapRendererDiagnostic);
    return MinimapOverlayRenderer::Initialize();
}

void DrainAutomapProjectionProbe() noexcept {
    std::array<MinimapProjectionSample,MinimapProjectionSampleCapacity> pending{};
    std::size_t count{},total{};bool full{};
    {
        std::lock_guard lock(AutomapProjectionSampleMutex);
        for(std::size_t i=AutomapProjectionSampleReported;
                i<AutomapProjectionSampleCount;++i)
            pending[count++]=AutomapProjectionSamples[i];
        AutomapProjectionSampleReported=AutomapProjectionSampleCount;
        total=AutomapProjectionSampleCount;
        full=AutomapProjectionSampleCount==MinimapProjectionSampleCapacity;
    }
    for(std::size_t i=0;i<count;++i) {
        const auto& row=pending[i];
        char code[5]{};CodeText(row.code,code);
        char line[520]{};
        std::snprintf(line,sizeof(line),
            "LOOT_MINIMAP_PROJECTION_SAMPLE version=1.0.0 code='%.4s' unitId=%u classId=%u mode=%u "
            "world=(%d,%d) client=(%d,%d) automap=(%d,%d) "
            "clip=(%d,%d,%d,%d) visible=%u itemAgeMs=%llu tid=%u "
            "projection=D2R+0xD4910 rendezvous=D2R+0xD76E0 mapSenseDependency=0 drawing=json-minimapIcon-rules",
            code,row.unitId,row.classId,row.mode,row.worldX,row.worldY,
            row.client.x,row.client.y,row.screen.x,row.screen.y,
            row.clip.left,row.clip.top,row.clip.width,row.clip.height,
            row.visible?1U:0U,
            static_cast<unsigned long long>(row.itemAgeMs),row.threadId);
        Emit(line);
    }
    if(count) {
        char line[400]{};
        std::snprintf(line,sizeof(line),
            "LOOT_MINIMAP_PROJECTION_STATUS version=1.0.0 samples=%zu passes=%llu projectCalls=%llu "
            "failures=%llu contention=%llu target=json-minimapIcon-rules itemFreshMs=%llu sampleIntervalMs=%llu "
            "mapSenseDependency=0 drawing=json-minimapIcon-rules minimapIconJson=SUPPORTED",
            total,
            static_cast<unsigned long long>(AutomapProjectionPasses.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(AutomapProjectionProjectCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(AutomapProjectionFailures.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(AutomapProjectionContention.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(MinimapProjectionItemFreshMilliseconds),
            static_cast<unsigned long long>(MinimapProjectionSampleIntervalMilliseconds));
        Emit(line);
        const auto renderer=MinimapOverlayRenderer::GetDiagnostics();
        char rendererLine[420]{};
        std::snprintf(rendererLine,sizeof(rendererLine),
            "LOOT_MINIMAP_RENDERER_STATUS version=1.0.0 backend=%s present=%llu queueCaptures=%llu "
            "initAttempts=%llu initFailures=%llu renderedFrames=%llu publishedFrames=%llu "
            "drawnMarkers=%llu automapSuppressedFrames=%llu lastInitFailureStage=%u hooksInstalled=%u queueReady=%u rendererReady=%u",
            MinimapOverlayRenderer::ActiveBackendName(),
            static_cast<unsigned long long>(renderer.presentCalls),
            static_cast<unsigned long long>(renderer.directQueueCaptures),
            static_cast<unsigned long long>(renderer.rendererInitAttempts),
            static_cast<unsigned long long>(renderer.rendererInitFailures),
            static_cast<unsigned long long>(renderer.renderedFrames),
            static_cast<unsigned long long>(renderer.publishedFrames),
            static_cast<unsigned long long>(renderer.drawnMarkers),
            static_cast<unsigned long long>(renderer.automapSuppressedFrames),
            renderer.lastInitFailureStage,
            renderer.hooksInstalled?1U:0U,
            renderer.commandQueueReady?1U:0U,
            renderer.rendererInitialized?1U:0U);
        Emit(rendererLine);
    }
    if(full && count)Emit("LOOT_MINIMAP_PROJECTION_FULL version=1.0.0 "
        "samples=96 reset=Ctrl+Shift+F6 diagnosticSamplingFull=1 markerProjectionContinues=1 drawing=1");
}


void ArmMinimapTracking() noexcept {
    ResetAutomapProjectionProbe();
    MinimapProbeArmed.store(true,std::memory_order_release);
}

void ObserveMinimapItemPosition(const void* nativeUnit,std::uint32_t rawCode,
    std::uint32_t unitId,std::uint32_t classId) noexcept {
    if(!MinimapProbeArmed.load(std::memory_order_acquire) ||
       !nativeUnit || !unitId || !PrintableItemCode(rawCode)) return;
    MinimapProbeSample sample{};
    sample.code=CanonicalItemCode(rawCode);
    sample.unitId=unitId;sample.classId=classId;
    std::array<std::uint8_t,MinimapUnitBytes> first{},last{};
    std::array<std::uint8_t,MinimapPathBytes> pathFirst{},pathLast{};
    SIZE_T copied{};
    if(!ReadProcessMemory(GetCurrentProcess(),nativeUnit,first.data(),
            first.size(),&copied) || copied!=first.size())
        sample.status=MinimapProbeStatus::UnitReadFailed;
    else {
        std::uint32_t type{},nativeClass{},nativeId{},mode{};
        std::memcpy(&type,first.data(),4);
        std::memcpy(&nativeClass,first.data()+4,4);
        std::memcpy(&nativeId,first.data()+8,4);
        std::memcpy(&mode,first.data()+12,4);
        sample.mode=mode;
        if(type!=4 || nativeClass!=classId || nativeId!=unitId ||
           (mode!=3 && mode!=5))return;
        std::uintptr_t pathAddress{};
        std::memcpy(&pathAddress,first.data()+0x38,sizeof(pathAddress));
        if(pathAddress==0)sample.status=MinimapProbeStatus::NullPath;
        else if(!GroundCandidatePageReadable(pathAddress,MinimapPathBytes))
            sample.status=MinimapProbeStatus::PathUnreadable;
        else {
            copied=0;
            if(!ReadProcessMemory(GetCurrentProcess(),
                    reinterpret_cast<const void*>(pathAddress),
                    pathFirst.data(),pathFirst.size(),&copied) ||
                copied!=pathFirst.size())
                sample.status=MinimapProbeStatus::PathUnreadable;
            else {
                copied=0;
                const bool pathAgain=ReadProcessMemory(GetCurrentProcess(),
                    reinterpret_cast<const void*>(pathAddress),
                    pathLast.data(),pathLast.size(),&copied) &&
                    copied==pathLast.size();
                copied=0;
                const bool unitAgain=ReadProcessMemory(GetCurrentProcess(),
                    nativeUnit,last.data(),last.size(),&copied) &&
                    copied==last.size();
                if(!pathAgain || !unitAgain ||
                   std::memcmp(pathFirst.data(),pathLast.data(),
                       pathFirst.size())!=0 ||
                   std::memcmp(first.data(),last.data(),0x10)!=0 ||
                   std::memcmp(first.data()+0x38,last.data()+0x38,
                       sizeof(pathAddress))!=0)
                    sample.status=MinimapProbeStatus::PathChanged;
                else {
                    const auto candidate=MinimapWorldPositionProbe::Decode(
                        pathFirst.data(),pathFirst.size());
                    sample.x=candidate.x;sample.y=candidate.y;
                    std::memcpy(sample.coordWindow.data(),
                        pathFirst.data()+0x10,sample.coordWindow.size());
                    sample.status=candidate.plausible?
                        MinimapProbeStatus::CandidateCoord:
                        MinimapProbeStatus::ImplausibleCoords;
                }
            }
        }
    }
    // Publish copied world coordinates into the standalone automap projection registry.
    UpdateMinimapProjectionItem(sample);
}




void ArmGroundPropertyEvidence() noexcept {
    GroundPropertyEvidenceArmed.store(false,std::memory_order_release);
    {
        std::lock_guard lock(GroundPropertyEvidenceMutex);
        GroundPropertyEvidenceRows.fill({});
        GroundPropertyEvidenceCount=0;
        GroundPropertyEvidenceReported=0;
    }
    GroundPropertyEvidenceReadFailed.store(0,std::memory_order_relaxed);
    GroundPropertyEvidenceContended.store(0,std::memory_order_relaxed);
    GroundPropertyEvidenceArmed.store(true,std::memory_order_release);
    Emit("LOOT_GROUND_PROPERTY_EVIDENCE_BEGIN version=1.0.0 "
         "trigger=Ctrl+Shift+F8 limit=12 nativeUnitWindow=0xC0 "
         "candidatePointerOffset=0x10 candidateWindow=0x100 "
         "candidateMeaning=UNVERIFIED readOnly=1 oneHop=1 "
         "noItemWrites=1 rulesUnchanged=1 pickupGuardUnchanged=1 "
         "captureExpiry=none");
}

void ObserveGroundPropertyEvidence(const void* nativeUnit,
    std::uint32_t code,const std::uint32_t (&header)[4]) noexcept {
    if (!GroundPropertyEvidenceArmed.load(std::memory_order_acquire) ||
        !nativeUnit || !PrintableItemCode(code) ||
        header[0]!=4 || header[2]==0 || header[3]!=3) return;
    if(!GroundPropertyEvidenceMutex.try_lock()) {
        GroundPropertyEvidenceContended.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    const auto duplicate=std::any_of(GroundPropertyEvidenceRows.begin(),
        GroundPropertyEvidenceRows.begin()+GroundPropertyEvidenceCount,
        [=](const GroundPropertyEvidence& sample) {
            return sample.unitId==header[2] && sample.code==code;
        });
    const bool full=GroundPropertyEvidenceCount>=GroundPropertyEvidenceCapacity;
    GroundPropertyEvidenceMutex.unlock();
    if(duplicate || full) return;
    GroundPropertyEvidence sample{};
    sample.code=code;sample.classId=header[1];
    sample.unitId=header[2];sample.mode=header[3];
    SIZE_T copied{};
    if(ReadProcessMemory(GetCurrentProcess(),nativeUnit,sample.bytes.data(),
        sample.bytes.size(),&copied) && copied==sample.bytes.size())
        sample.bytesRead=static_cast<std::uint32_t>(copied);
    else {
        copied=0;
        if(!ReadProcessMemory(GetCurrentProcess(),nativeUnit,
            sample.bytes.data(),GroundPropertyEvidenceFallbackBytes,&copied) ||
            copied!=GroundPropertyEvidenceFallbackBytes) {
            GroundPropertyEvidenceReadFailed.fetch_add(1,
                std::memory_order_relaxed);
            return;
        }
        sample.bytesRead=static_cast<std::uint32_t>(copied);
    }
    // The 0.2.36 log showed an instance-varying aligned qword at +0x10;
    // reading a single bounded target is diagnostic, not a type assertion.
    constexpr std::size_t candidateOffset=0x10;
    static_assert(candidateOffset+sizeof(std::uintptr_t)<=
        GroundPropertyEvidenceFallbackBytes);
    std::memcpy(&sample.candidateAddress,
        sample.bytes.data()+candidateOffset,sizeof(sample.candidateAddress));
    if(sample.candidateAddress<0x10000 ||
        (sample.candidateAddress&7U)!=0 ||
        sample.candidateAddress>
            std::numeric_limits<std::uintptr_t>::max()-GroundPropertyCandidateBytes)
        sample.candidateStatus=GroundPropertyCandidateStatus::ZeroOrNoncanonical;
    else {
        constexpr std::array<std::size_t,3> lengths{{0x100,0x80,0x40}};
        bool pageOk=false;
        for(const auto len:lengths) {
            if(!GroundCandidatePageReadable(sample.candidateAddress,len))
                continue;
            pageOk=true;copied=0;
            if(ReadProcessMemory(GetCurrentProcess(),
                    reinterpret_cast<const void*>(sample.candidateAddress),
                    sample.candidateBytes.data(),len,&copied) &&
                    copied==len) {
                sample.candidateBytesRead=static_cast<std::uint32_t>(len);
                sample.candidateStatus=GroundPropertyCandidateStatus::ReadOk;
                break;
            }
        }
        if(sample.candidateStatus!=GroundPropertyCandidateStatus::ReadOk)
            sample.candidateStatus=pageOk ?
                GroundPropertyCandidateStatus::ReadFailed :
                GroundPropertyCandidateStatus::BadPage;
    }
    // Confirm identity AND candidate pointer still match the pre-read unit;
    // never log a cross-item byte set as coherent evidence.
    std::array<std::uint8_t,0x18> after{};
    copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),nativeUnit,after.data(),
            after.size(),&copied) || copied!=after.size() ||
        std::memcmp(after.data(),sample.bytes.data(),after.size())!=0) {
        GroundPropertyEvidenceReadFailed.fetch_add(1,
            std::memory_order_relaxed);
        return;
    }
    if(!GroundPropertyEvidenceMutex.try_lock()) {
        GroundPropertyEvidenceContended.fetch_add(1,
            std::memory_order_relaxed);
        return;
    }
    const bool stillArmed=GroundPropertyEvidenceArmed.load(
        std::memory_order_relaxed);
    const bool seen=std::any_of(GroundPropertyEvidenceRows.begin(),
        GroundPropertyEvidenceRows.begin()+GroundPropertyEvidenceCount,
        [=](const GroundPropertyEvidence& row) {
            return row.unitId==sample.unitId && row.code==sample.code;
        });
    if(stillArmed && !seen &&
       GroundPropertyEvidenceCount<GroundPropertyEvidenceCapacity) {
        GroundPropertyEvidenceRows[GroundPropertyEvidenceCount++]=sample;
        if(GroundPropertyEvidenceCount==GroundPropertyEvidenceCapacity)
            GroundPropertyEvidenceArmed.store(false,
                std::memory_order_release);
    }
    GroundPropertyEvidenceMutex.unlock();
}

// Optional authoritative post-pickup comparison. SDK ItemInfo belongs to
// inventory/cursor and is copied on the SDK callback thread. Only a matching
// runtimeId AND canonical item code is paired with an earlier ground sample.
// This DOES NOT attest that any byte offset inside candidate data means quality.
void ObserveGroundPropertyCarriedSdk(const D2RL::Items::ItemInfo* info) noexcept {
    if(!info || info->structSize<D2RL::Items::ItemInfoRequiredSize ||
       info->runtimeId==0 ||
       info->container==D2RL::Items::ItemContainer::Ground ||
       !GroundPropertyEvidenceMutex.try_lock()) return;
    for(std::size_t i=0;i<GroundPropertyEvidenceCount;++i) {
        auto& row=GroundPropertyEvidenceRows[i];
        if(row.unitId==info->runtimeId &&
           row.code==CanonicalItemCode(info->code) && !row.sdk.matched) {
            row.sdk.matched=true;
            row.sdk.code=info->code;
            row.sdk.runtimeId=info->runtimeId;
            row.sdk.classId=info->classId;
            row.sdk.quality=static_cast<std::uint32_t>(info->quality);
            row.sdk.itemLevel=info->itemLevel;
            row.sdk.sockets=info->socketCount;
            row.sdk.stateFlags=info->stateFlags;
            row.sdk.container=static_cast<std::uint32_t>(info->container);
            // Copy the evidence snapshot while holding the same mutex as the
            // SDK identity match. Never dereference candidateAddress here.
            // A failed/short native read remains explicitly unavailable.
            if(row.candidateStatus==GroundPropertyCandidateStatus::ReadOk)
                row.sdk.candidateSnapshot=GroundCandidateProbe::Decode(
                    row.candidateBytes.data(),row.candidateBytesRead);
            break;
        }
    }
    GroundPropertyEvidenceMutex.unlock();
}

// Worker-only emission. No formatting, I/O, game callbacks, or native pointer
// retention occurs on the formatter thread. Worker never dereferences addr.
void DrainGroundPropertyEvidence() noexcept {
    std::array<GroundPropertyEvidence,GroundPropertyEvidenceCapacity> pending{};
    std::size_t count{};
    bool complete{};
    {
        std::lock_guard lock(GroundPropertyEvidenceMutex);
        for(std::size_t i=GroundPropertyEvidenceReported;
            i<GroundPropertyEvidenceCount;++i)
            pending[count++]=GroundPropertyEvidenceRows[i];
        GroundPropertyEvidenceReported=GroundPropertyEvidenceCount;
        complete=GroundPropertyEvidenceCount==GroundPropertyEvidenceCapacity;
    }
    for(std::size_t i=0;i<count;++i) {
        const auto& sample=pending[i];
        char codeText[5]{};
        CodeText(sample.code,codeText);
        char message[420]{};
        std::snprintf(message,sizeof(message),
            "LOOT_GROUND_PROPERTY_SAMPLE version=1.0.0 "
            "code='%.4s' classId=%u unitId=%u mode=%u "
            "readBytes=%u source=verified-inner-writer "
            "quality=UNKNOWN ilvl=UNKNOWN sockets=UNKNOWN "
            "ethereal=UNKNOWN identified=UNKNOWN ruleInputsUnchanged=1",
            codeText,sample.classId,sample.unitId,sample.mode,
            sample.bytesRead);
        Emit(message);
        for(std::size_t offset=0;offset<sample.bytesRead;offset+=32) {
            char hex[32*3+1]{};
            const auto len=std::min<std::size_t>(32,
                sample.bytesRead-offset);
            for(std::size_t j=0;j<len;++j)
                std::snprintf(hex+j*3,sizeof(hex)-j*3,
                    "%02X ",static_cast<unsigned>(sample.bytes[offset+j]));
            std::snprintf(message,sizeof(message),
                "LOOT_GROUND_PROPERTY_BYTES version=1.0.0 "
                "code='%.4s' unitId=%u unitOffset=0x%02zX hex='%s' "
                "offsetsUnqualified=1",codeText,sample.unitId,offset,hex);
            Emit(message);
        }
        std::snprintf(message,sizeof(message),
            "LOOT_GROUND_PROPERTY_CANDIDATE version=1.0.0 "
            "code='%.4s' unitId=%u pointerField=unit+0x10 "
            "status=%u readBytes=%u pointerValue=REDACTED "
            "itemDataMeaning=UNVERIFIED decoder=none ruleInputsUnchanged=1",
            codeText,sample.unitId,
            static_cast<unsigned>(sample.candidateStatus),
            sample.candidateBytesRead);
        Emit(message);
        // Two candidate scalar positions from the 0.2.38 log. Report raw
        // values as evidence; never fill RuleEngine::Item from them.
        const auto candidates=sample.candidateStatus==
            GroundPropertyCandidateStatus::ReadOk ?
            GroundCandidateProbe::Decode(sample.candidateBytes.data(),
                sample.candidateBytesRead) : GroundCandidateProbe::Snapshot{};
        if(candidates.available) {
            std::snprintf(message,sizeof(message),
                "LOOT_GROUND_PROPERTY_CANDIDATE_FIELDS version=1.0.0 "
                "code='%.4s' unitId=%u qualityAt00=%u levelAt38=%u "
                "rawFlagsAt18=0x%X candidateOnly=1 "
                "groundSnapshot=1 sdkConfirmed=0 rulesUnchanged=1",
                codeText,sample.unitId,candidates.qualityCandidate,
                candidates.levelCandidate,candidates.rawFlagsCandidate);
            Emit(message);
        }
        for(std::size_t offset=0;offset<sample.candidateBytesRead;offset+=32) {
            char hex[32*3+1]{};
            const auto len=std::min<std::size_t>(32,
                sample.candidateBytesRead-offset);
            for(std::size_t j=0;j<len;++j)
                std::snprintf(hex+j*3,sizeof(hex)-j*3,
                    "%02X ",static_cast<unsigned>(sample.candidateBytes[offset+j]));
            std::snprintf(message,sizeof(message),
                "LOOT_GROUND_PROPERTY_CANDIDATE_BYTES version=1.0.0 "
                "code='%.4s' unitId=%u candidateOffset=0x%02zX hex='%s' "
                "offsetsUnqualified=1",codeText,sample.unitId,offset,hex);
            Emit(message);
        }
    }
    if(complete && count)
        Emit("LOOT_GROUND_PROPERTY_EVIDENCE_FULL version=1.0.0 "
             "limit=12 rearm=Ctrl+Shift+F8 readerNotPromoted=1");
}

void DrainGroundPropertySdkEvidence() noexcept {
    std::array<GroundPropertySdkEvidence,GroundPropertyEvidenceCapacity> pending{};
    std::size_t count{};
    {
        std::lock_guard lock(GroundPropertyEvidenceMutex);
        for(std::size_t i=0;i<GroundPropertyEvidenceCount;++i) {
            auto& row=GroundPropertyEvidenceRows[i];
            if(row.sdk.matched && row.sdk.runtimeId) {
                pending[count++]=row.sdk;
                row.sdk.runtimeId=0; // reported, do not repeat
            }
        }
    }
    for(std::size_t i=0;i<count;++i) {
        const auto& sdk=pending[i];
        char codeText[5]{};
        CodeText(sdk.code,codeText);
        char message[430]{};
        std::snprintf(message,sizeof(message),
            "LOOT_GROUND_PROPERTY_SDK_MATCH version=1.0.0 "
            "code='%.4s' runtimeId=%u classId=%u "
            "quality=%u ilvl=%u sockets=%u stateFlags=0x%X "
            "identified=%u ethereal=%u container=%u "
            "match=runtime-id-plus-code postPickupOnly=1 "
            "candidateOffsetsUnqualified=1 filterUnchanged=1",
            codeText,sdk.runtimeId,sdk.classId,sdk.quality,
            sdk.itemLevel,sdk.sockets,sdk.stateFlags,
            (sdk.stateFlags&D2RL::Items::ItemStateIdentified)?1U:0U,
            (sdk.stateFlags&D2RL::Items::ItemStateEthereal)?1U:0U,
            sdk.container);
        Emit(message);
        // Compare the F8 *ground-time* snapshot with SDK post-pickup facts.
        // Don't print zero as a value if a candidate read was unavailable.
        // This F8 capture compares raw flags only; the separately guarded
        // active ethereal decoder uses just the selected 0x00400000 bit.
        const auto comparison=GroundCandidateProbe::Compare(
            sdk.candidateSnapshot,sdk.quality,sdk.itemLevel);
        char quality[18]{},level[18]{},flags[18]{};
        if(comparison.available) {
            std::snprintf(quality,sizeof(quality),"%u",
                sdk.candidateSnapshot.qualityCandidate);
            std::snprintf(level,sizeof(level),"%u",
                sdk.candidateSnapshot.levelCandidate);
            std::snprintf(flags,sizeof(flags),"0x%X",
                sdk.candidateSnapshot.rawFlagsCandidate);
        } else {
            std::snprintf(quality,sizeof(quality),"UNKNOWN");
            std::snprintf(level,sizeof(level),"UNKNOWN");
            std::snprintf(flags,sizeof(flags),"UNKNOWN");
        }
        std::snprintf(message,sizeof(message),
            "LOOT_GROUND_PROPERTY_SDK_COMPARE version=1.0.0 "
            "code='%.4s' runtimeId=%u candidateQualityAt00=%s "
            "sdkQuality=%u qualityResult=%s candidateLevelAt38=%s "
            "sdkIlvl=%u levelResult=%s candidateFlagsAt18=%s "
            "flagsMeaning=UNVERIFIED snapshot=ground-time "
            "sdk=post-pickup readOnly=1 rulesUnchanged=1",
            codeText,sdk.runtimeId,quality,sdk.quality,
            !comparison.available?"UNAVAILABLE":
                comparison.qualityEqualsSdk?"MATCH":"MISMATCH",
            level,sdk.itemLevel,
            !comparison.available?"UNAVAILABLE":
                comparison.levelEqualsSdk?"MATCH":"MISMATCH",flags);
        Emit(message);
    }
}

// Auto-armed, bounded 0.2.48 ground-label latency evidence. No new native
// hooks and NO logging or allocation in renderer/formatter/game-thread hooks.
// The worker drains copied scalars and QPC timestamps every ~200 ms. This
// measures first PLUGIN observation, not the unhooked actual item-drop event.
constexpr std::uint32_t SacredArmorCode =
    static_cast<std::uint32_t>('u') |
    (static_cast<std::uint32_t>('a') << 8U) |
    (static_cast<std::uint32_t>('r') << 16U);
enum class LootLatencyStage : std::uint8_t {
    ObserveFormatter, ObserveSoE, ReadBegin, ReadUnknown, ReadOk,
    MatchSound, MatchStyle, MatchBulk, QueueSound, NativeSound,
    StyleText, BulkPaint, HoverPaint, Count
};
constexpr std::array<const char*,static_cast<std::size_t>(LootLatencyStage::Count)>
    LootLatencyStageNames{{
        "OBSERVE_FORMATTER", "OBSERVE_SOE", "READ_BEGIN", "READ_UNKNOWN",
        "READ_OK", "MATCH_SOUND", "MATCH_STYLE", "MATCH_BULK",
        "QUEUE_SOUND", "NATIVE_SOUND_RETURN", "STYLE_TEXT_READY",
        "BULK_PAINT_FORWARDED", "HOVER_PAINT_FORWARDED"}};
struct LootLatencyItem final {
    std::uint32_t id{},code{};
    std::int64_t firstQpc{};
    std::uint32_t readAttempts{},unknownReads{};
    bool haveReadValue{};
    std::uint32_t lastQuality{},lastIlvl{};
    // Log the first failed ground-header gate and bounded state changes,
    // rather than suppressing all 84 early rejections as one READ_UNKNOWN.
    std::array<char,48> lastUnknownGate{};
    std::uint32_t unknownGateTransitions{};
    std::uint64_t phases{};
};
struct LootLatencyEvent final {
    std::int64_t qpc{};
    std::uint32_t id{},code{},readAttempts{},unknownReads{};
    std::uint64_t elapsedUs{},readDurationUs{};
    LootLatencyStage stage{};
    std::uint32_t quality{},ilvl{};
    std::array<char,48> detail{};
};
constexpr std::size_t LootLatencyMaxItems=12;
constexpr std::size_t LootLatencyMaxEvents=384;
std::atomic_bool LootLatencyArmed{};
std::mutex LootLatencyMutex{};
std::array<LootLatencyItem,LootLatencyMaxItems> LootLatencyItems{};
std::array<LootLatencyEvent,LootLatencyMaxEvents> LootLatencyEvents{};
std::size_t LootLatencyItemCount{},LootLatencyEventCount{};
std::atomic<std::uint32_t> LootLatencyLost{};

void ArmLootLatency() noexcept {
    // Temporarily disarm before resetting session-owned arrays. A render
    // callback might finish its previous transaction; only copied data lives
    // here, so never wait on a native item pointer or hold a native hook lock.
    LootLatencyArmed.store(false,std::memory_order_release);
    { std::lock_guard lock(LootLatencyMutex);
      LootLatencyItems.fill({});LootLatencyEvents.fill({});
      LootLatencyItemCount=0;LootLatencyEventCount=0; }
    LootLatencyLost.store(0,std::memory_order_relaxed);
    LootLatencyArmed.store(true,std::memory_order_release);
    Emit("LOOT_LATENCY_BEGIN version=1.0.0 hotkey=Ctrl+Shift+F7 "
         "codes=uar,divo maxDistinctItems=12 maxEvents=384 "
         "clock=QueryPerformanceCounter origin=first-plugin-observation "
         "not-native-drop-event=1 logIo=worker-only readOnly=1");
}
void TraceLootLatency(std::uint32_t rawCode,std::uint32_t id,
    LootLatencyStage stage,const char* detail="",
    std::uint64_t readDurationUs=0,std::uint32_t quality=0,
    std::uint32_t ilvl=0) noexcept {
    if (!LootLatencyArmed.load(std::memory_order_acquire) || !id) return;
    const auto code=CanonicalItemCode(rawCode);
    if(code!=SacredArmorCode && code!=DivineCode)return;
    LARGE_INTEGER stamp{},frequency{};
    if(!QueryPerformanceCounter(&stamp) ||
       !QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0) return;
    if(!LootLatencyMutex.try_lock()) {
        LootLatencyLost.fetch_add(1,std::memory_order_relaxed);return;
    }
    LootLatencyItem* item=nullptr;
    for(std::size_t i=0;i<LootLatencyItemCount;++i)
        if(LootLatencyItems[i].id==id && LootLatencyItems[i].code==code) {
            item=&LootLatencyItems[i];break;
        }
    if(!item && LootLatencyItemCount<LootLatencyMaxItems) {
        item=&LootLatencyItems[LootLatencyItemCount++];
        item->id=id;item->code=code;item->firstQpc=stamp.QuadPart;
    }
    if(!item) { LootLatencyMutex.unlock();return; }
    if(stage==LootLatencyStage::ReadBegin) ++item->readAttempts;
    if(stage==LootLatencyStage::ReadUnknown) ++item->unknownReads;
    // The first FAILED read and the first SUCCESSFUL later retry are both
    // captured. READ_OK is additionally emitted whenever the verified scalar
    // value changes for the same ground identity. This distinguishes a slow
    // reader from a valid-but-not-final quality value without logging every
    // render pass.
    const auto bit=1ULL<<static_cast<unsigned>(stage);
    bool readValueChanged=false;
    bool unknownGateChanged=false;
    if(stage==LootLatencyStage::ReadUnknown &&
       item->unknownGateTransitions<12U) {
        const char* why=detail?detail:"";
        if(item->unknownGateTransitions==0U ||
           std::strncmp(item->lastUnknownGate.data(),why,
               item->lastUnknownGate.size()-1U)!=0) {
            std::snprintf(item->lastUnknownGate.data(),
                item->lastUnknownGate.size(),"%s",why);
            ++item->unknownGateTransitions;
            unknownGateChanged=true;
        }
    }
    if(stage==LootLatencyStage::ReadOk) {
        readValueChanged=item->haveReadValue &&
            (item->lastQuality!=quality || item->lastIlvl!=ilvl);
        if(!item->haveReadValue || readValueChanged) {
            item->haveReadValue=true;
            item->lastQuality=quality;
            item->lastIlvl=ilvl;
        }
    }
    if(((item->phases&bit)!=0 && !readValueChanged &&
        !unknownGateChanged) ||
       LootLatencyEventCount>=LootLatencyMaxEvents) {
        if(LootLatencyEventCount>=LootLatencyMaxEvents)
            LootLatencyLost.fetch_add(1,std::memory_order_relaxed);
        LootLatencyMutex.unlock();return;
    }
    item->phases|=bit;
    auto& e=LootLatencyEvents[LootLatencyEventCount++];
    e.qpc=stamp.QuadPart;e.id=id;e.code=code;e.stage=stage;
    e.readAttempts=item->readAttempts;e.unknownReads=item->unknownReads;
    e.elapsedUs=stamp.QuadPart>=item->firstQpc ?
        static_cast<std::uint64_t>((stamp.QuadPart-item->firstQpc)*
            1000000LL/frequency.QuadPart) : 0;
    e.readDurationUs=readDurationUs;e.quality=quality;e.ilvl=ilvl;
    if(readValueChanged)
        std::snprintf(e.detail.data(),e.detail.size(),"verified-value-changed");
    else if(detail) std::snprintf(e.detail.data(),e.detail.size(),"%s",detail);
    LootLatencyMutex.unlock();
}
void DrainLootLatency() noexcept {
    std::array<LootLatencyEvent,LootLatencyMaxEvents> events{};
    std::size_t count{};
    if(!LootLatencyMutex.try_lock())return;
    count=LootLatencyEventCount;
    if(count)std::copy_n(LootLatencyEvents.begin(),count,events.begin());
    LootLatencyEventCount=0;
    LootLatencyMutex.unlock();
    for(std::size_t i=0;i<count;++i) {
        const auto& e=events[i];
        char code[5]{};CodeText(e.code,code);
        char line[415]{};
        std::snprintf(line,sizeof(line),
            "LOOT_LATENCY_STAGE version=1.0.0 stage=%s code='%.4s' "
            "unitId=%u qpc=%lld sinceFirstObserveUs=%llu "
            "readDurationUs=%llu quality=%u ilvl=%u "
            "readAttempts=%u unknownReads=%u detail=%s "
            "observationNotDrop=1",
            LootLatencyStageNames[static_cast<std::size_t>(e.stage)],
            code,e.id,static_cast<long long>(e.qpc),
            static_cast<unsigned long long>(e.elapsedUs),
            static_cast<unsigned long long>(e.readDurationUs),
            e.quality,e.ilvl,e.readAttempts,e.unknownReads,e.detail.data());
        Emit(line);
    }
    const auto lost=LootLatencyLost.exchange(0,std::memory_order_relaxed);
    if(lost) {
        char line[150]{};
        std::snprintf(line,sizeof(line),
            "LOOT_LATENCY_DROPPED version=1.0.0 count=%u "
            "note=contention-or-full-event-buffer",lost);
        Emit(line);
    }
}

// Build 93847 native ground reader promoted after exact id+code post-pickup
// SDK comparisons (quality 3/6/4, ilvl 84/84/99). Read-only; no handles are
// forged from a native pointer; only the qualified ethereal bit is decoded
// when the first potentially matching rule requests it.
// Every active read uses the same bounded one-hop source as the F8 probe.
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
    const auto type=GroundCandidateProbe::ReadLe32(before.data());
    const auto classId=GroundCandidateProbe::ReadLe32(before.data()+4);
    const auto id=GroundCandidateProbe::ReadLe32(before.data()+8);
    const auto mode=GroundCandidateProbe::ReadLe32(before.data()+12);
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
    if(GroundCandidateProbe::ReadLe32(itemData.data())!=
           GroundCandidateProbe::ReadLe32(check.data()) ||
       GroundCandidateProbe::ReadLe32(itemData.data()+0x38)!=
           GroundCandidateProbe::ReadLe32(check.data()+0x38))
        return fail("quality-or-level-changed-during-read");
    // Flag snapshot is required only when a potentially matching flag rule
    // requests it. Quality/ilvl-only rules avoid the flag gate entirely.
    if((includeEthereal || includeIdentified) &&
       GroundCandidateProbe::ReadLe32(itemData.data()+0x18)!=
           GroundCandidateProbe::ReadLe32(check.data()+0x18))
        return fail("item-flags-changed-during-read");
    const auto quality=GroundCandidateProbe::ReadLe32(itemData.data());
    const auto level=GroundCandidateProbe::ReadLe32(itemData.data()+0x38);
    auto scalars=GroundPropertyLive::Validate(quality,level);
    if(includeEthereal && scalars.qualityKnown && scalars.itemLevelKnown) {
        scalars.etherealKnown=true;
        scalars.ethereal=GroundEthereal::FromNativeFlags(
            GroundCandidateProbe::ReadLe32(itemData.data()+0x18));
        if(mode==GroundPropertyLive::PresentingMode)
            GroundEtherealMode5Reads.fetch_add(1,std::memory_order_relaxed);
    }
    if(includeIdentified && scalars.qualityKnown && scalars.itemLevelKnown) {
        scalars.identifiedKnown=true;
        scalars.identified=GroundIdentified::FromNativeFlags(
            GroundCandidateProbe::ReadLe32(itemData.data()+0x18));
        if(mode==GroundPropertyLive::PresentingMode)
            GroundIdentifiedMode5Reads.fetch_add(1,std::memory_order_relaxed);
    }
    if((!scalars.qualityKnown || !scalars.itemLevelKnown) && reason)
        *reason="quality-or-level-out-of-range";
    else if(reason && mode==GroundPropertyLive::PresentingMode)
        *reason="ok-mode5-verified-label";
    return scalars;
}

// The opt-in ethereal/identified probe remains available independently of
// active JSON rule reads. Each active bit is decoded only after
// both native item-data snapshots and identity have passed their guards.
// SDK ground/post-pickup correlation is still available for verification.
constexpr std::size_t EtherealProbeMaxItems=12;
constexpr std::size_t EtherealProbeMaxSamples=EtherealProbeMaxItems*2;
struct EtherealProbeRow {
    std::uint32_t code{},unitId{},classId{},mode{};
    std::uint32_t flagsAt18{},qualityAt00{},levelAt38{};
    std::uint32_t sdkStateFlags{},sdkQuality{},sdkContainer{};
    bool rawKnown{},sdkMatched{},reported{},sdkReported{};
};
std::atomic_bool EtherealProbeArmed{};
std::mutex EtherealProbeMutex;
std::array<EtherealProbeRow,EtherealProbeMaxSamples> EtherealProbeRows{};
std::size_t EtherealProbeCount{};
std::atomic<std::uint32_t> EtherealProbeContended{};

void ArmEtherealProbe() noexcept {
    EtherealProbeArmed.store(false,std::memory_order_release);
    {
        std::lock_guard lock(EtherealProbeMutex);
        EtherealProbeRows.fill({});EtherealProbeCount=0;
    }
    EtherealProbeContended.store(0,std::memory_order_relaxed);
    EtherealProbeArmed.store(true,std::memory_order_release);
    Emit("LOOT_ETHEREAL_PROBE_ARMED version=1.0.0 trigger=Ctrl+Shift+F8 "
         "source=verified-ground-label-mode5-or-mode3 "
         "candidate=itemData+0x18 raw-flags-no-bit-assumption "
         "sdk=post-pickup-runtimeId+code+classId stateFlags "
         "maxItems=12 maxModesPerItem=2 "
         "etherealJson=SUPPORTED mask=0x00400000 readOnly=1 pickupUnchanged=1");
    // Reuse the SAME guarded flags samples and carried-SDK identity pairing;
    // independent identified diagnostics, no second scan or extra native hook.
    Emit("LOOT_IDENTIFIED_PROBE_ARMED version=1.0.0 trigger=Ctrl+Shift+F8 "
         "source=shared-guarded-ethereal-flags-capture modes=verified-label-5+3 "
         "candidate=itemData+0x18 bit=0x00000010 QUALIFIED "
         "sdk=post-pickup-runtimeId+code+classId stateFlags "
         "maxItems=12 maxModesPerItem=2 identifiedJson=SUPPORTED mask=0x00000010 "
         "readOnly=1 pickupUnchanged=1");
}

void ObserveEtherealProbe(const void* unit,std::uint32_t code,
    std::uint32_t expectedId,std::uint32_t expectedClassId) noexcept {
    if(!EtherealProbeArmed.load(std::memory_order_acquire) ||
       !unit || !code || !expectedId || !Context ||
       !D2RL::GetBuildName(Context) ||
       std::string_view(D2RL::GetBuildName(Context))!="93847") return;
    code=CanonicalItemCode(code);
    std::array<std::uint8_t,0x18> before{},after{};
    SIZE_T copied{};
    if(!ReadProcessMemory(GetCurrentProcess(),unit,before.data(),
            before.size(),&copied) || copied!=before.size()) return;
    const auto type=GroundCandidateProbe::ReadLe32(before.data());
    const auto classId=GroundCandidateProbe::ReadLe32(before.data()+4);
    const auto id=GroundCandidateProbe::ReadLe32(before.data()+8);
    const auto mode=GroundCandidateProbe::ReadLe32(before.data()+12);
    if(type!=4 || id!=expectedId || classId!=expectedClassId ||
       !GroundPropertyLive::AllowsMode(mode,
           GroundPropertyLive::Purpose::VerifiedLabel)) return;
    // Reject duplicate captures before reading candidate data; never hold a
    // mutex during ReadProcessMemory or an engine/SDK callback.
    if(!EtherealProbeMutex.try_lock()) {
        EtherealProbeContended.fetch_add(1,std::memory_order_relaxed);return;
    }
    bool duplicate=false,seenIdentity=false;
    std::size_t identities=0;
    for(std::size_t i=0;i<EtherealProbeCount;++i) {
        const auto& row=EtherealProbeRows[i];
        if(row.unitId==id && row.code==code && row.classId==classId) {
            seenIdentity=true;
            if(row.mode==mode)duplicate=true;
        }
        bool first=true;
        for(std::size_t j=0;j<i;++j)
            if(EtherealProbeRows[j].unitId==row.unitId &&
               EtherealProbeRows[j].code==row.code &&
               EtherealProbeRows[j].classId==row.classId) {
                first=false;break;
            }
        if(first)++identities;
    }
    const bool full=EtherealProbeCount>=EtherealProbeMaxSamples ||
        (!seenIdentity && identities>=EtherealProbeMaxItems);
    EtherealProbeMutex.unlock();
    if(duplicate || full)return;
    std::uintptr_t address{};
    std::memcpy(&address,before.data()+0x10,sizeof(address));
    constexpr std::size_t length=0x40;
    std::array<std::uint8_t,length> first{},second{};
    bool known=GroundCandidatePageReadable(address,length);
    if(known) {
        copied=0;
        known=ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(address),first.data(),
            first.size(),&copied) && copied==first.size();
    }
    copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),unit,after.data(),
        after.size(),&copied) || copied!=after.size() || before!=after)
        return; // transient/reused unit is not a valid ground sample
    if(known) {
        copied=0;
        known=ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(address),second.data(),
            second.size(),&copied) && copied==second.size();
    }
    std::uint32_t raw{},quality{},level{};
    if(known) {
        raw=GroundCandidateProbe::ReadLe32(first.data()+0x18);
        quality=GroundCandidateProbe::ReadLe32(first.data());
        level=GroundCandidateProbe::ReadLe32(first.data()+0x38);
        known=raw==GroundCandidateProbe::ReadLe32(second.data()+0x18) &&
              quality==GroundCandidateProbe::ReadLe32(second.data()) &&
              level==GroundCandidateProbe::ReadLe32(second.data()+0x38) &&
              quality>=1 && quality<=9 && level>=1 && level<=99;
    }
    if(!EtherealProbeMutex.try_lock()) {
        EtherealProbeContended.fetch_add(1,std::memory_order_relaxed);return;
    }
    duplicate=false;seenIdentity=false;identities=0;
    for(std::size_t i=0;i<EtherealProbeCount;++i) {
        const auto& row=EtherealProbeRows[i];
        if(row.unitId==id && row.code==code && row.classId==classId) {
            seenIdentity=true;
            if(row.mode==mode)duplicate=true;
        }
        bool firstIdentity=true;
        for(std::size_t j=0;j<i;++j)
            if(EtherealProbeRows[j].unitId==row.unitId &&
               EtherealProbeRows[j].code==row.code &&
               EtherealProbeRows[j].classId==row.classId) {
                firstIdentity=false;break;
            }
        if(firstIdentity)++identities;
    }
    if(!duplicate && EtherealProbeCount<EtherealProbeMaxSamples &&
       (seenIdentity || identities<EtherealProbeMaxItems)) {
        auto& row=EtherealProbeRows[EtherealProbeCount++];
        row.code=code;row.unitId=id;row.classId=classId;row.mode=mode;
        row.rawKnown=known;
        if(known) {
            row.flagsAt18=raw;row.qualityAt00=quality;row.levelAt38=level;
        }
    }
    EtherealProbeMutex.unlock();
}

void ObserveEtherealProbeCarriedSdk(
    const D2RL::Items::ItemInfo* info) noexcept {
    if(!info || info->structSize<D2RL::Items::ItemInfoRequiredSize ||
       !info->runtimeId ||
       info->container==D2RL::Items::ItemContainer::Ground ||
       !EtherealProbeMutex.try_lock())return;
    for(std::size_t i=0;i<EtherealProbeCount;++i) {
        auto& row=EtherealProbeRows[i];
        if(row.unitId==info->runtimeId &&
           row.code==CanonicalItemCode(info->code) &&
           row.classId==info->classId && !row.sdkMatched) {
            row.sdkMatched=true;row.sdkStateFlags=info->stateFlags;
            row.sdkQuality=static_cast<std::uint32_t>(info->quality);
            row.sdkContainer=static_cast<std::uint32_t>(info->container);
        }
    }
    EtherealProbeMutex.unlock();
}

void DrainEtherealProbe() noexcept {
    std::array<EtherealProbeRow,EtherealProbeMaxSamples> pending{};
    std::size_t count{};
    if(!EtherealProbeMutex.try_lock())return;
    for(std::size_t i=0;i<EtherealProbeCount;++i) {
        auto& row=EtherealProbeRows[i];
        if(!row.reported || (row.sdkMatched && !row.sdkReported)) {
            pending[count++]=row;row.reported=true;
            if(row.sdkMatched)row.sdkReported=true;
        }
    }
    EtherealProbeMutex.unlock();
    for(std::size_t i=0;i<count;++i) {
        const auto& row=pending[i];
        char code[5]{};CodeText(row.code,code);
        char line[515]{};
        if(row.sdkMatched) {
            const bool nativeEthereal=GroundEthereal::FromNativeFlags(
                row.flagsAt18);
            const bool sdkEthereal=(row.sdkStateFlags &
                D2RL::Items::ItemStateEthereal)!=0U;
            const char* maskCompare=!row.rawKnown ? "UNAVAILABLE" :
                (nativeEthereal==sdkEthereal ? "MATCH" : "MISMATCH");
            std::snprintf(line,sizeof(line),
                "LOOT_ETHEREAL_PROBE_SDK_COMPARE version=1.0.0 "
                "code='%.4s' unitId=%u classId=%u mode=%u "
                "rawFlagsAt18=0x%08X candidateKnown=%u "
                "qualityAt00=%u ilvlAt38=%u "
                "sdkStateFlags=0x%08X sdkEthereal=%u sdkQuality=%u "
                "sdkContainer=%u selectedMask=0x00400000 "
                "nativeEthereal=%u selectedMaskSdkCompare=%s "
                "rawRemainingBits=UNQUALIFIED postPickup=1 "
                "etherealJson=SUPPORTED mask=0x00400000",
                code,row.unitId,row.classId,row.mode,row.flagsAt18,
                row.rawKnown?1U:0U,row.qualityAt00,row.levelAt38,
                row.sdkStateFlags,
                (row.sdkStateFlags&D2RL::Items::ItemStateEthereal)?1U:0U,
                row.sdkQuality,row.sdkContainer,
                nativeEthereal?1U:0U,maskCompare);
        } else {
            std::snprintf(line,sizeof(line),
                "LOOT_ETHEREAL_PROBE_SAMPLE version=1.0.0 "
                "code='%.4s' unitId=%u classId=%u mode=%u "
                "rawFlagsAt18=0x%08X candidateKnown=%u "
                "qualityAt00=%u ilvlAt38=%u "
                "sdk=AWAITING_PICKUP bitMeaning=UNQUALIFIED "
                "etherealJson=SUPPORTED mask=0x00400000",
                code,row.unitId,row.classId,row.mode,row.flagsAt18,
                row.rawKnown?1U:0U,row.qualityAt00,row.levelAt38);
        }
        Emit(line);
        // The probe separately corroborates the active bit decoder; both
        // identified and unidentified unique Sacred Armors matched ground
        // control has established either the bit or its ground-time stability.
        // Never promote this comparison to JSON filtering in this build.
        char identifiedLine[515]{};
        constexpr std::uint32_t CandidateIdentifiedBit=
            GroundIdentified::NativeIdentifiedMask;
        const bool candidateIdentified=
            (row.flagsAt18&CandidateIdentifiedBit)!=0U;
        if(row.sdkMatched) {
            const bool sdkIdentified=(row.sdkStateFlags &
                D2RL::Items::ItemStateIdentified)!=0U;
            const char* compare=!row.rawKnown ? "UNAVAILABLE" :
                (candidateIdentified==sdkIdentified ? "MATCH" : "MISMATCH");
            std::snprintf(identifiedLine,sizeof(identifiedLine),
                "LOOT_IDENTIFIED_PROBE_SDK_COMPARE version=1.0.0 "
                "code='%.4s' unitId=%u classId=%u mode=%u "
                "rawFlagsAt18=0x%08X candidateKnown=%u "
                "candidateMask=0x00000010 candidateIdentified=%u "
                "sdkStateFlags=0x%08X sdkIdentified=%u "
                "sdkQuality=%u sdkContainer=%u candidateVsSdk=%s "
                "bitMeaning=QUALIFIED postPickup=1 identifiedJson=SUPPORTED mask=0x00000010",
                code,row.unitId,row.classId,row.mode,row.flagsAt18,
                row.rawKnown?1U:0U,candidateIdentified?1U:0U,
                row.sdkStateFlags,sdkIdentified?1U:0U,
                row.sdkQuality,row.sdkContainer,compare);
        } else {
            std::snprintf(identifiedLine,sizeof(identifiedLine),
                "LOOT_IDENTIFIED_PROBE_SAMPLE version=1.0.0 "
                "code='%.4s' unitId=%u classId=%u mode=%u "
                "rawFlagsAt18=0x%08X candidateKnown=%u "
                "candidateMask=0x00000010 candidateIdentified=%u "
                "sdk=AWAITING_PICKUP bitMeaning=UNQUALIFIED "
                "identifiedJson=SUPPORTED mask=0x00000010",
                code,row.unitId,row.classId,row.mode,row.flagsAt18,
                row.rawKnown?1U:0U,candidateIdentified?1U:0U);
        }
        Emit(identifiedLine);
    }
    const auto contention=EtherealProbeContended.exchange(0,
        std::memory_order_relaxed);
    if(contention) {
        char line[180]{};
        std::snprintf(line,sizeof(line),
            "LOOT_ETHEREAL_PROBE_CONTENTION version=1.0.0 count=%u "
            "samples-may-be-incomplete=1",contention);
        Emit(line);
    }
}

// The same loader-owned, qualified GetUnitStat bridge as stack quantity;
// no native pointer is retained, no direct/unqualified D2R call is made.
// The early mode-5 exception is restricted by the caller's VerifiedLabel
// purpose and the same stable type/class/ID/mode header as the quality read.
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
        SocketProbeCandidateStatId,0);
    copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),nativeUnit,after.data(),
        sizeof(after),&copied) || copied!=sizeof(after) ||
       before!=after) return false;
    // Two matching scalar reads protect against a transient stat result.
    const auto second=getter(const_cast<void*>(nativeUnit),
        SocketProbeCandidateStatId,0);
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
    RuleEngine::Item item{}; item.code=CanonicalItemCode(code);
    // Capture a temporarily unavailable ground unit BEFORE the native
    // quality reader can run. Otherwise a first failed observation would
    // be invisible in the timeline and falsely look like slow reading.
    const bool propertyPotential=expectedId && table &&
        (table->usesQuality || table->usesItemLevel || table->usesSockets ||
         table->usesEthereal || table->usesIdentified) &&
        RuleEngine::NextNativeProperty(table->rules,item)!=
            RuleEngine::NextProperty::None;
    if (!nativeUnit) {
        if(propertyPotential)TraceLootLatency(item.code,expectedId,
            LootLatencyStage::ReadUnknown,"native-unit-null");
        return item;
    }
    std::array<std::uint32_t,4> header{};
    SIZE_T copied{};
    const bool headerRead=ReadProcessMemory(GetCurrentProcess(),
        nativeUnit,header.data(),sizeof(header),&copied) &&
        copied==sizeof(header);
    if (!headerRead || header[0]!=4 ||
        !GroundPropertyLive::AllowsMode(header[3],purpose) ||
        !header[2] || (expectedId && header[2]!=expectedId)) {
        if(propertyPotential) {
            if(!headerRead)TraceLootLatency(item.code,expectedId,
                LootLatencyStage::ReadUnknown,"unit-header-read-failed");
            else {
                // Existing admission policy stays unchanged. The 0.2.45
                // trace showed 84 pre-reader rejections over 714 ms; copy
                // bounded header scalars (never pointers) to identify whether
                // a transition in type/mode/ID explains that interval.
                char gate[48]{};
                std::snprintf(gate,sizeof(gate),
                    "gate t=%u m=%u id=%u exp=%u",header[0],header[3],
                    header[2],expectedId);
                TraceLootLatency(item.code,expectedId,
                    LootLatencyStage::ReadUnknown,gate);
            }
        }
        return item;
    }
    item.classIdKnown=true; item.classId=header[1];
    if (table && table->usesQuantity &&
        GroundQuantityReader.load(std::memory_order_acquire)) {
        const auto quantity=GroundStackQuantity(nativeUnit);
        item.quantityKnown=true; item.quantity=quantity>1?quantity:1;
    }
    // Evaluate only the first relevant missing property, then re-evaluate
    // the ordered rules. A first code-only rule never invokes a reader.
    // Four lazy property groups at most: quality/ilvl, sockets, ethereal and identified.
    // A code-only match never invokes any native property reader.
    for(unsigned propertyGroup=0;propertyGroup<4 && table;++propertyGroup) {
        const auto next=RuleEngine::NextNativeProperty(table->rules,item);
        if(next==RuleEngine::NextProperty::None) break;
        if(next==RuleEngine::NextProperty::Ethereal) {
            GroundEtherealRuleReads.fetch_add(1,std::memory_order_relaxed);
            TraceLootLatency(item.code,header[2],LootLatencyStage::ReadBegin,
                header[3]==GroundPropertyLive::PresentingMode ?
                    "mode5-ethereal-verified-label" : "requires-ethereal");
            LARGE_INTEGER readStart{},readStop{},readFreq{};
            const bool timeRead=LootLatencyArmed.load(std::memory_order_relaxed) &&
                QueryPerformanceCounter(&readStart) &&
                QueryPerformanceFrequency(&readFreq) && readFreq.QuadPart>0;
            const char* reason="unknown";
            const auto fields=ReadNativeGroundQualityLevel(nativeUnit,
                header[2],header[1],purpose,&reason,true);
            const bool haveReadEnd=timeRead && QueryPerformanceCounter(&readStop);
            const std::uint64_t readDuration=haveReadEnd &&
                readStop.QuadPart>=readStart.QuadPart ?
                static_cast<std::uint64_t>((readStop.QuadPart-readStart.QuadPart)*
                    1000000LL/readFreq.QuadPart) : 0;
            TraceLootLatency(item.code,header[2],
                fields.etherealKnown ? LootLatencyStage::ReadOk :
                LootLatencyStage::ReadUnknown,
                fields.etherealKnown ? (fields.ethereal ?
                    "ethereal-true" : "ethereal-false") : reason,
                readDuration,fields.quality,fields.itemLevel);
            if(!fields.etherealKnown) {
                GroundEtherealRuleUnknown.fetch_add(1,
                    std::memory_order_relaxed);
                break; // unknown cannot mean nonethereal or bypass Show
            }
            item.etherealKnown=true;
            item.ethereal=fields.ethereal;
            // This same guarded snapshot also validates quality and ilvl;
            // preserve them for any later rule without another native read.
            item.qualityKnown=fields.qualityKnown;
            item.quality=fields.quality;
            item.itemLevelKnown=fields.itemLevelKnown;
            item.itemLevel=fields.itemLevel;
            continue;
        }
        if(next==RuleEngine::NextProperty::Identified) {
            GroundIdentifiedRuleReads.fetch_add(1,std::memory_order_relaxed);
            TraceLootLatency(item.code,header[2],LootLatencyStage::ReadBegin,
                header[3]==GroundPropertyLive::PresentingMode ?
                    "mode5-identified-verified-label" : "requires-identified");
            LARGE_INTEGER readStart{},readStop{},readFreq{};
            const bool timeRead=LootLatencyArmed.load(std::memory_order_relaxed) &&
                QueryPerformanceCounter(&readStart) &&
                QueryPerformanceFrequency(&readFreq) && readFreq.QuadPart>0;
            const char* reason="unknown";
            const auto fields=ReadNativeGroundQualityLevel(nativeUnit,
                header[2],header[1],purpose,&reason,false,true);
            const bool haveReadEnd=timeRead && QueryPerformanceCounter(&readStop);
            const std::uint64_t readDuration=haveReadEnd &&
                readStop.QuadPart>=readStart.QuadPart ?
                static_cast<std::uint64_t>((readStop.QuadPart-readStart.QuadPart)*
                    1000000LL/readFreq.QuadPart) : 0;
            TraceLootLatency(item.code,header[2],
                fields.identifiedKnown ? LootLatencyStage::ReadOk :
                LootLatencyStage::ReadUnknown,
                fields.identifiedKnown ? (fields.identified ?
                    "identified-true" : "identified-false") : reason,
                readDuration,fields.quality,fields.itemLevel);
            if(!fields.identifiedKnown) {
                GroundIdentifiedRuleUnknown.fetch_add(1,
                    std::memory_order_relaxed);
                break; // Unknown does not mean unidentified; no later hide.
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
            if(!known) GroundSocketRuleUnknown.fetch_add(1,
                std::memory_order_relaxed);
            item.socketsKnown=known;
            if(known) item.sockets=count;
            if(!known) break; // fail open; never fall through to broad hide
            continue;
        }
        if(table && (table->usesQuality || table->usesItemLevel) &&
           RuleEngine::NeedsNativeQualityLevel(table->rules,item)) {
        GroundPropertyLiveReads.fetch_add(1,std::memory_order_relaxed);
        TraceLootLatency(item.code,header[2],LootLatencyStage::ReadBegin,
            header[3]==GroundPropertyLive::PresentingMode ?
                "mode5-verified-label" : "requires-quality-or-item-level");
        LARGE_INTEGER readStart{},readStop{},readFreq{};
        const bool timeRead=LootLatencyArmed.load(std::memory_order_relaxed) &&
            QueryPerformanceCounter(&readStart) &&
            QueryPerformanceFrequency(&readFreq) && readFreq.QuadPart>0;
        const char* readReason="unknown";
        const auto fields=ReadNativeGroundQualityLevel(
            nativeUnit,header[2],header[1],purpose,&readReason);
        const bool haveReadEnd=timeRead && QueryPerformanceCounter(&readStop);
        const std::uint64_t readDuration=haveReadEnd &&
            readStop.QuadPart>=readStart.QuadPart ?
            static_cast<std::uint64_t>((readStop.QuadPart-readStart.QuadPart)*
                1000000LL/readFreq.QuadPart) : 0;
        TraceLootLatency(item.code,header[2],
            fields.qualityKnown && fields.itemLevelKnown ?
            LootLatencyStage::ReadOk : LootLatencyStage::ReadUnknown,
            readReason,readDuration,fields.quality,fields.itemLevel);
        if((table->usesQuality && !fields.qualityKnown) ||
           (table->usesItemLevel && !fields.itemLevelKnown))
            GroundPropertyLiveUnknown.fetch_add(1,std::memory_order_relaxed);
        item.qualityKnown=fields.qualityKnown;
        item.quality=fields.quality;
        item.itemLevelKnown=fields.itemLevelKnown;
        item.itemLevel=fields.itemLevel;
        if(!fields.qualityKnown || !fields.itemLevelKnown) break;
        } // qualified quality/ilvl reader
    } // ordered property groups
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
std::atomic<std::uint64_t> InWorldScopeGlyphCalls{};
std::atomic<std::uint64_t> InWorldScopePainterCalls{};
std::atomic<std::uint64_t> InWorldScopeRgbaSamples{};
std::atomic<std::uint64_t> InWorldScopeMatchingRule{};
std::atomic<std::uint32_t> InWorldScopeLastId{};
std::atomic<std::uint32_t> InWorldScopeLastCode{};
std::mutex InWorldScopeSampleMutex{};
std::array<float,4> InWorldScopeLastRgba{};
std::atomic<ULONGLONG> InWorldScopeFirstHoverMs{};
std::atomic_bool InWorldScopeReported{};
void ResetInWorldScopeProbe() noexcept;
void ReportInWorldScopeProbe() noexcept;

// 0.1.99: event-correlated, automatically timed read-only renderer comparison.
// V1 supplies a matching-item heartbeat; drawing can happen AFTER V3 exits.
// Only existing glyph-B/ground-paint hooks are sampled. No new hooks or SoE changes.
constexpr ULONGLONG HoverDiffMaximumMs = HoverDiffPolicy::MaximumMs;
constexpr std::size_t HoverDiffMaxSites = 32;
constexpr std::size_t HoverDiffStackFrames = 10;
constexpr std::size_t HoverDiffMaxStacks = 2;
using HoverDiffPhase = HoverDiffPolicy::Phase;
enum class HoverDiffRenderer : std::uint8_t { Glyph, Painter };
struct HoverDiffSite final {
    std::uintptr_t callerRva{};
    DWORD thread{};
    std::uint64_t hoverSamples{};
    std::uint64_t awaySamples{};
    float firstX{}, firstY{};
    std::array<float,4> firstRgba{};
    bool rgbaValid{};
};
// 0.1.99: bounded 32-unit glyph coordinate bins. Hover-vs-away counts
// are only correlation: native draw labels, other UI, and cursor changes may
// all contribute. No name or item is attributed on coordinates alone.
constexpr float HoverDiffTileSize=32.0f;
constexpr std::size_t HoverDiffMaxTiles=512;
constexpr std::size_t HoverDiffPrintTiles=32;
struct HoverDiffTile final {
    std::int32_t binX{},binY{};
    std::uint32_t hover{},away{};
    float sampleX{},sampleY{};
    // 0.1.99: collect a bounded color witness from glyph-B during hover;
    // these are renderer input values, not proof that a tile belongs to the item.
    std::array<float,4> hoverRgba{};
    bool hoverRgbaValid{};
};
struct HoverDiffStack final {
    std::array<std::uintptr_t,HoverDiffStackFrames> frames{};
    std::uint16_t count{};
    DWORD thread{};
    std::uintptr_t callerRva{};
};
struct HoverDiffRendererState final {
    std::mutex mutex{};
    std::array<HoverDiffSite,HoverDiffMaxSites> sites{};
    std::array<HoverDiffTile,HoverDiffMaxTiles> tiles{};
    std::size_t tileCount{};
    std::uint64_t invalidTileCoords{};
    std::uint64_t tileOverflow{};
    std::array<std::array<HoverDiffStack,HoverDiffMaxStacks>,2> stacks{};
    std::array<std::size_t,2> stackCounts{};
    std::size_t siteCount{};
    std::atomic<std::uint64_t> observed{};
    std::atomic<std::uint64_t> hover{};
    std::atomic<std::uint64_t> away{};
    std::atomic<std::uint64_t> transition{};
    std::atomic<std::uint64_t> contention{};
    std::atomic<std::uint64_t> overflow{};
};
HoverDiffRendererState HoverDiffGlyph{};
HoverDiffRendererState HoverDiffPainter{};
std::atomic<ULONGLONG> HoverDiffBeginMs{};
std::atomic<ULONGLONG> HoverDiffLastMatchingMs{};
std::atomic<std::uint64_t> HoverDiffMatches{};
std::atomic<std::uint64_t> HoverDiffOtherItemCallbacks{};
std::atomic<std::uint32_t> HoverDiffItemId{};
std::atomic<std::uint32_t> HoverDiffCode{};
std::atomic<std::uint64_t> HoverDiffEpoch{1};
std::atomic_bool HoverDiffCompleted{};
// 0.1.99 diagnostic-only, fail-closed RGB proof-of-concept. The geometry is
// learned from a complete first-hover/away comparison, never hard-coded.
// It is NOT a production item-ownership proof: other UI can overlap the region.
// Keep the experimental tile-based RGB path as inert provenance research.
// 0.1.86 visually proved its 32-unit horizontal training cluster can cover
// only part of a hidden-hover item name (e.g. DIVINE O colored, RB native).
// Do not arm it automatically or forward partial-color glyphs. Reenable only
// after the full label is qualified by native item/element ownership.
constexpr bool HoverRgbTrialEnabled = false;
std::atomic_bool HoverRgbTrialReady{};
std::atomic<std::int32_t> HoverRgbTrialMinX{};
std::atomic<std::int32_t> HoverRgbTrialMaxX{};
std::atomic<std::int32_t> HoverRgbTrialRowY{};
std::atomic<std::uint32_t> HoverRgbTrialItemId{};
std::atomic<std::uint32_t> HoverRgbTrialCode{};
std::atomic<std::uint64_t> HoverRgbTrialRegionHits{};
std::atomic<std::uint64_t> HoverRgbTrialForwarded{};
std::atomic<std::uint64_t> HoverRgbTrialRejectedColor{};
std::atomic<std::uint64_t> HoverRgbTrialNoRule{};
std::atomic<std::uint64_t> HoverRgbTrialNotFresh{};
std::atomic<std::uint64_t> HoverRgbTrialWrongItem{};
void ResetHoverDifferentialProbe() noexcept;
// Opt-in read-only native row observation. No row or rectangle hook at startup.
void ResetNativeRowRuntime() noexcept;
void ResetNativeRowLiveSession() noexcept;
void EnableAutomaticNativeHover() noexcept;

void ObserveHoverDifferentialDraw(HoverDiffRenderer renderer,
    std::uintptr_t caller, float x=0.0f, float y=0.0f,
    const float* rgba=nullptr) noexcept;

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
    if(rule && (rule->hasTextColor || rule->hasBackground))
        TraceLootLatency(code,event->unitId,LootLatencyStage::MatchStyle,
            "soe-v2-style-rule-match",0,observed.quality,observed.itemLevel);
    // Quantity is independent of JSON rules: e.g. an unfiltered stack of
    // consumables still gets "3x ". Stat 0/1 retains vanilla display.
    const auto quantity=GroundStackQuantity(event->nativeUnit);
    const char palette=rule && rule->hasTextColor ?
        HoverStyle::PaletteSelector(rule->textColor):'\0';
    if (rule && rule->hasTextColor && !palette)
        InWorldStyleUnsupportedColor.fetch_add(1,std::memory_order_relaxed);
    if (rule && InWorldRenderScopeApi.load(std::memory_order_acquire)) {
        ULONGLONG unstarted{};
        (void)InWorldScopeFirstHoverMs.compare_exchange_strong(
            unstarted,GetTickCount64(),std::memory_order_acq_rel);
    }
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
    if(rule && rule->hasTextColor)
        TraceLootLatency(code,event->unitId,LootLatencyStage::StyleText,
            "soe-v2-replacement-ready");
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
            Emit("LOOT_INWORLD_BACKEND version=1.0.0 mode=soe-interop-v1 owner=soe hook=0xC0420 probeHooksAdded=0 textObserve=1 style=optional-v2");
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
    ArmMinimapTracking();
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
CollectionHelperFn OriginalCollectionHelper{};
std::atomic_bool CollectionHookInstalled{false};
std::atomic<std::uint64_t> CollectionTotal{};
std::atomic<std::uint64_t> CollectionContended{};

std::atomic<int> ActivePhase{-1};
std::atomic<ULONGLONG> Deadline{};
std::mutex PhasesMutex;
// Claims a single pre-call snapshot for each phase.  These atomics are reset
// only when the capture is no longer armed and after outstanding callbacks are
// guarded by the phase mutex.
std::array<std::atomic_bool, MaximumPhases> BeforeClaimed{};

struct Row {
    std::uintptr_t callerRva{};
    std::uint32_t code{};
    std::uint32_t hits{};
    std::uintptr_t firstItem{};
    std::uintptr_t lastItem{};
    std::uint32_t threadId{};
};
struct TextVariant {
    char escaped[CandidateTextMaximum * 4 + 1]{};
    std::uint32_t samples{};
};
struct SlotCandidate {
    bool readable{};
    bool terminated{};
    // Raw prefix bytes plus bounded text bytes, never followed pointers.
    std::array<std::uint8_t, CandidateTextOffset> prefix{};
    std::array<std::uint8_t, CandidateTextMaximum> text{};
};
struct SlotSurvey {
    bool attempted{};
    std::uint32_t readable{};
    std::uint32_t textCandidates{};
    std::uint64_t atHelperHit{};
    std::array<SlotCandidate, CandidateSlotCount> slots{};
};
struct CollectionSample {
    // Snapshot at the first call before and after the collection helper,
    // then a late periodic post-call survey. This DOES NOT establish which
    // candidate records are currently rendered versus stale spare entries.
    SlotSurvey firstBeforeSlots{};
    SlotSurvey firstAfterSlots{};
    SlotSurvey lastAfterSlots{};
    std::uint32_t surveyClaimFailures{};
    std::uint32_t surveyPeriodicFailures{};
    std::uint32_t surveyUpdates{};
    // A first-hit pre/post read of 0x144 bytes at the *argument storage*,
    // never a pointer followed from that storage. Not a verified item record.
    bool beforeArg3Ok{};
    bool afterArg3Ok{};
    std::array<std::uint8_t, RecordBytes> beforeArg3{};
    std::array<std::uint8_t, RecordBytes> afterArg3{};
    std::uint32_t preClaimFailures{};
    std::uint32_t candidateReadFailures{};
    std::uint32_t textSamples{};
    std::uint32_t invalidTextSamples{};
    std::uint32_t variantOverflow{};
    std::array<TextVariant, CandidateTextVariants> variants{};
    std::size_t variantCount{};
    // Retain 0.1.9's 64-byte post-call snapshots for compatibility.
    bool arg1SnapshotOk{};
    bool arg3SnapshotOk{};
    std::array<std::uint64_t, 8> arg1Snapshot{};
    std::array<std::uint64_t, 8> arg3Snapshot{};
    std::uint64_t hits{};
    std::uint64_t firstLimit{};
    std::uint64_t lastLimit{};
    std::uint64_t minimumLimit{};
    std::uint64_t maximumLimit{};
    std::uintptr_t firstCaller{};
    std::uintptr_t lastCaller{};
    std::uintptr_t firstArg1{};
    std::uintptr_t firstArg2{};
    std::uintptr_t firstArg3{};
    std::uint32_t firstThread{};
    std::uint32_t stackCount{};
    std::array<std::uintptr_t, 8> stack{};
};
struct FormatterObservation {
    bool claimed{};
    bool preOk{};
    bool postOk{};
    bool unitOk{};
    bool codeAttempted{};
    bool codeGuardPassed{};
    bool codeValid{};
    std::uint32_t codeValue{};
    std::uintptr_t nativeUnit{};
    std::uintptr_t dest{};
    std::uintptr_t record{};
    std::uintptr_t caller{};
    std::uint32_t hits{};
    std::uint32_t threadId{};
    std::uint32_t fourthArg{};
    std::uint64_t fifthArg{};
    std::uint64_t sixthArg{};
    std::uint8_t result{};
    std::uint32_t nativeFirst6[6]{};
    std::array<std::uint8_t, RecordBytes> pre{};
    std::array<std::uint8_t, RecordBytes> post{};
};
struct FormatterPhase {
    std::uint64_t calls{}; // qualified source-site + paired record calls
    std::uint64_t sourceCallHits{};
    std::uint64_t seen{}; // every formatter entry during an active phase
    std::uint64_t sourceSiteHits{}; // both verified return addresses
    std::uint64_t pairedHits{}; // destination == record + 0x24
    std::uintptr_t firstReturnAddress{}; // any formatter entry, for diagnostics
    std::uint64_t skipped{};
    std::size_t rowCount{};
    std::array<FormatterObservation, FormatterSlotsPerPhase> rows{};
};
struct Phase {
    char name[32]{};
    std::array<Row, MaximumRows> rows{};
    std::size_t rowCount{};
    std::uint64_t calls{};
    std::uint64_t retained{};
    std::uint64_t contended{};
    std::uint64_t overflow{};
    std::uint32_t stackCount{};
    std::array<std::uintptr_t, 8> divineStack{};
    CollectionSample collection{};
    FormatterPhase formatter{};
    GeometryMode geometryMode{GeometryMode::Off};
};
std::array<Phase, MaximumPhases> Phases{};
std::size_t PhaseCount{};
std::atomic<std::uint64_t> GlobalContended{};

// Independent, explicit capture output.  D2RLoader owns pluginLogPath; never
// overwrite it or rely on the loader flushing its log on crash/exit.


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

// Only loader lifecycle / configuration paths emit messages; neither native
// hover hook writes files or logs. Legacy capture callers remain no-ops.
void CaptureLine(const char*) noexcept {}
void FlushCapture() noexcept {}
void Emit(const char* message) noexcept {
    if (!Context || !message) return;

    // Production logging only. Historical reverse-engineering probes still
    // exist in source where they document qualified native contracts, but
    // their verbose sample/status output is no longer routed to the loader.
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

// 0.1.52: separate, short-lived, read-only TOOLTIP ROUTE diagnostic.
// We do NOT claim 0x36EF50 creates tooltips: it is a verified one-argument
// item-code accessor that previous captures showed in hover/inventory stacks.
// Capture bounded native return-address stacks for the Divine Orb only,
// while a user performs exactly one UI action. This is NOT a native drop hook.
constexpr ULONGLONG TooltipRouteCaptureMs=12'000;
constexpr std::size_t TooltipRouteMaxSignatures=20;
constexpr std::size_t TooltipRouteMaxFrames=12;
std::atomic_bool TooltipRouteArmed{false};
std::atomic<ULONGLONG> TooltipRouteDeadline{};
std::atomic<std::uint64_t> TooltipRouteHits{};
std::atomic<std::uint64_t> TooltipRouteContended{};
std::atomic<std::uint64_t> TooltipRouteSkipped{};
std::atomic<std::uint64_t> TooltipRouteOverflow{};
std::array<char,24> TooltipRoutePhase{};
struct TooltipRouteSignature {
    std::array<std::uintptr_t,TooltipRouteMaxFrames> frames{};
    std::uint32_t frameCount{};
    std::uintptr_t callerRva{};
    std::uint32_t threadId{};
    std::uint32_t itemIdCandidate{};
    std::uint64_t hits{};
};
std::mutex TooltipRouteMutex;
std::array<TooltipRouteSignature,TooltipRouteMaxSignatures> TooltipRouteRows{};
std::size_t TooltipRouteCount{};

void ObserveTooltipRoute(void* item,std::uint32_t code,
    std::uintptr_t caller) noexcept {
    if (code!=DivineCode ||
        !TooltipRouteArmed.load(std::memory_order_relaxed) ||
        GetTickCount64()>=TooltipRouteDeadline.load(std::memory_order_relaxed)) return;
    const auto hit=TooltipRouteHits.fetch_add(1,std::memory_order_relaxed)+1;
    // Bound the unwind work to approximately eight samples per 60fps second.
    if ((hit-1)%8!=0) {TooltipRouteSkipped.fetch_add(1,std::memory_order_relaxed);return;}
    if(!TooltipRouteMutex.try_lock()) {
        TooltipRouteContended.fetch_add(1,std::memory_order_relaxed);return;
    }
    std::lock_guard<std::mutex> hold(TooltipRouteMutex,std::adopt_lock);
    if(!TooltipRouteArmed.load(std::memory_order_relaxed) ||
       GetTickCount64()>=TooltipRouteDeadline.load(std::memory_order_relaxed)) return;
    PVOID nativeFrames[TooltipRouteMaxFrames]{};
    const auto depth=CaptureStackBackTrace(0,
        static_cast<DWORD>(TooltipRouteMaxFrames),nativeFrames,nullptr);
    std::array<std::uintptr_t,TooltipRouteMaxFrames> frames{};
    for(std::uint32_t i=0;i<depth;++i) {
        const auto absolute=reinterpret_cast<std::uintptr_t>(nativeFrames[i]);
        frames[i]=(absolute>=Base && absolute-Base<ImageSize)?absolute-Base:0;
    }
    const auto callerRva=caller>=Base && caller-Base<ImageSize?caller-Base:0;
    TooltipRouteSignature* existing{};
    for(std::size_t i=0;i<TooltipRouteCount;++i) {
        auto& record=TooltipRouteRows[i];
        if(record.callerRva!=callerRva || record.frameCount!=depth) continue;
        // Frame 0 may point into this observer; discriminate by the native
        // caller chain from frame 2 onward, not by plugin code addresses.
        bool equal=true;
        for(std::uint32_t j=2;j<depth;++j)
            if(record.frames[j]!=frames[j]) {equal=false;break;}
        if(equal) {existing=&record;break;}
    }
    if(!existing) {
        if(TooltipRouteCount>=TooltipRouteMaxSignatures) {
            TooltipRouteOverflow.fetch_add(1,std::memory_order_relaxed);return;
        }
        existing=&TooltipRouteRows[TooltipRouteCount++];
        *existing={};
        existing->frames=frames;
        existing->frameCount=depth;
        existing->callerRva=callerRva;
        existing->threadId=GetCurrentThreadId();
        if(item) {
            std::uint32_t header[4]{};
            SIZE_T copied{};
            if(ReadProcessMemory(GetCurrentProcess(),item,header,
                sizeof(header),&copied) && copied==sizeof(header) && header[0]==4)
                existing->itemIdCandidate=header[2];
        }
    }
    ++existing->hits;
}

void StartTooltipRoute(std::string_view phase) noexcept {
    if(phase!="hidden" && phase!="inventory" && phase!="idle" &&
       phase!="visible") {
        Emit("LOOT_TOOLTIP_REFUSED usage: tooltip-route-start idle|hidden|inventory|visible");return;
    }
    if(!HookInstalled.load(std::memory_order_acquire) ||
       !OriginalGetItemCode || !Context || !D2RL::GetBuildName(Context) ||
       std::string_view(D2RL::GetBuildName(Context))!="93847") {
        Emit("LOOT_TOOLTIP_REFUSED code-helper-not-qualified-or-build-mismatch");return;
    }
    TooltipRouteArmed.store(false,std::memory_order_release);
    {
        std::lock_guard<std::mutex> guard(TooltipRouteMutex);
        TooltipRouteRows.fill({});TooltipRouteCount=0;
        TooltipRouteHits.store(0);TooltipRouteSkipped.store(0);
        TooltipRouteContended.store(0);TooltipRouteOverflow.store(0);
        TooltipRoutePhase.fill(0);
        std::memcpy(TooltipRoutePhase.data(),phase.data(),phase.size());
        TooltipRouteDeadline.store(GetTickCount64()+TooltipRouteCaptureMs,
            std::memory_order_release);
        TooltipRouteArmed.store(true,std::memory_order_release);
    }
    char message[350]{};
    std::snprintf(message,sizeof(message),
        "LOOT_TOOLTIP_ROUTE_ARMED version=1.0.0 phase='%s' durationMs=%llu code='divo' nativeSource=0x36EF50-trampoline capture=caller+bounded-return-stack labelsRequired=0 modifications=0",
        TooltipRoutePhase.data(),
        static_cast<unsigned long long>(TooltipRouteCaptureMs));
    Emit(message);
}

void ReportTooltipRoute(bool stop) noexcept {
    if(stop) TooltipRouteArmed.store(false,std::memory_order_release);
    std::lock_guard<std::mutex> guard(TooltipRouteMutex);
    char message[460]{};
    std::snprintf(message,sizeof(message),
        "LOOT_TOOLTIP_ROUTE_BEGIN version=1.0.0 phase='%s' armed=%u divoHelperCalls=%llu sampled=%llu routes=%zu overflow=%llu contended=%llu source=item-code-accessor-not-tooltip-constructor",
        TooltipRoutePhase.data(),TooltipRouteArmed.load()?1U:0U,
        static_cast<unsigned long long>(TooltipRouteHits.load()),
        static_cast<unsigned long long>(TooltipRouteHits.load()-TooltipRouteSkipped.load()),
        TooltipRouteCount,
        static_cast<unsigned long long>(TooltipRouteOverflow.load()),
        static_cast<unsigned long long>(TooltipRouteContended.load()));
    Emit(message);
    for(std::size_t i=0;i<TooltipRouteCount;++i) {
        const auto& r=TooltipRouteRows[i];
        std::snprintf(message,sizeof(message),
            "LOOT_TOOLTIP_ROUTE phase='%s' route=%zu caller=D2R+0x%llX sampledHits=%llu frames=%u tid=%u itemIdCandidate=%u",
            TooltipRoutePhase.data(),i,
            static_cast<unsigned long long>(r.callerRva),
            static_cast<unsigned long long>(r.hits),r.frameCount,
            r.threadId,r.itemIdCandidate);
        Emit(message);
        for(std::uint32_t j=0;j<r.frameCount;++j) {
            std::snprintf(message,sizeof(message),
                "LOOT_TOOLTIP_FRAME phase='%s' route=%zu index=%u return=%s0x%llX",
                TooltipRoutePhase.data(),i,j,r.frames[j]?"D2R+":"non-D2R-or-unresolved:",
                static_cast<unsigned long long>(r.frames[j]));
            Emit(message);
        }
    }
    Emit("LOOT_TOOLTIP_ROUTE_END note=compare-hidden-inventory-idle-return-chains;no-native-tooltip-writes;no-drop-hook");
    FlushCapture();
}

// v0.1.52 — isolated observer of the COMMON hover/inventory producer at
// D2R+0xC7670.  The two observed native call sites supply RCX=output context
// and RDX=source object; no claim that the function is a tooltip renderer.
// Preserve the other integer register arguments as opaque pass-through values.
// This opt-in experiment does not touch the output, RDX item, or game data.
// The existing ReadSafe implementation appears later in the translation unit.
bool ReadSafe(std::uintptr_t rva,void* output,std::size_t count) noexcept;
constexpr std::uintptr_t TooltipProducerRva=0xC7670;
using TooltipProducerFn=void*(__fastcall*)(void*,void*,void*,void*) noexcept;
TooltipProducerFn OriginalTooltipProducer{};
std::atomic_bool TooltipProducerInstalled{false};
std::atomic_bool TooltipProducerArmed{false};
std::atomic<ULONGLONG> TooltipProducerDeadline{};
std::atomic<std::uint64_t> TooltipProducerCalls{};
std::atomic<std::uint64_t> TooltipProducerSamples{};
std::atomic<std::uint64_t> TooltipProducerContended{};
std::atomic<std::uint64_t> TooltipProducerOverflow{};
// Every call to each of the four static xrefs is counted even if the
// less frequent caller never coincides with the 1-in-8 snapshot sample.
constexpr std::array<std::uintptr_t,4> TooltipProducerReturns{{
    0x14FA04,0x14FC51,0x15BBE9,0x1509E1F}};
std::array<std::atomic<std::uint64_t>,4> TooltipProducerSiteCalls{};
std::atomic<std::uint64_t> TooltipProducerOtherCalls{};
std::array<char,24> TooltipProducerPhase{};
constexpr std::size_t TooltipProducerMaxRows=12;
struct TooltipProducerRow {
    std::uintptr_t callerRva{};
    std::uint32_t tid{};
    std::uint32_t outputHeader[4]{};
    std::uint32_t sourceHeader[4]{};
    std::uintptr_t resultPointer{};
    std::uint64_t hits{};
    std::uint64_t sampled{};
    bool outputReadable{};
    bool sourceReadable{};
};
std::mutex TooltipProducerMutex;
std::array<TooltipProducerRow,TooltipProducerMaxRows> TooltipProducerRows{};
std::size_t TooltipProducerRowCount{};

bool TooltipProducerReadHeader(void* p,std::uint32_t (&dest)[4]) noexcept {
    if (!p) return false;
    SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(),p,dest,sizeof(dest),&read) &&
           read==sizeof(dest);
}

void* __fastcall HookTooltipProducer(void* output,void* source,
    void* opaqueR8,void* opaqueR9) noexcept {
    const auto absolute=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto caller=(absolute>=Base && absolute-Base<ImageSize)?absolute-Base:0;
    // Call original ONCE with all four integer argument registers forwarded.
    // Do not call any other hooked helper while its native frame is active.
    const auto result=OriginalTooltipProducer(output,source,opaqueR8,opaqueR9);
    if(!TooltipProducerArmed.load(std::memory_order_relaxed) ||
       GetTickCount64()>=TooltipProducerDeadline.load(std::memory_order_relaxed))
        return result;
    const auto hits=TooltipProducerCalls.fetch_add(1,std::memory_order_relaxed)+1;
    bool known=false;
    for(std::size_t i=0;i<TooltipProducerReturns.size();++i)
        if(caller==TooltipProducerReturns[i]) {
            TooltipProducerSiteCalls[i].fetch_add(1,std::memory_order_relaxed);
            known=true;break;
        }
    if(!known) TooltipProducerOtherCalls.fetch_add(1,std::memory_order_relaxed);
    // Sample at most every eighth call, never log on the native hook thread.
    if((hits-1)%8) return result;
    if(!TooltipProducerMutex.try_lock()) {
        TooltipProducerContended.fetch_add(1,std::memory_order_relaxed);
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(TooltipProducerMutex,std::adopt_lock);
        if(!TooltipProducerArmed.load(std::memory_order_relaxed)) return result;
        TooltipProducerSamples.fetch_add(1,std::memory_order_relaxed);
        TooltipProducerRow* row{};
        for(std::size_t i=0;i<TooltipProducerRowCount;++i)
            if(TooltipProducerRows[i].callerRva==caller) {
                row=&TooltipProducerRows[i];break;
            }
        if(!row) {
            if(TooltipProducerRowCount>=TooltipProducerMaxRows) {
                TooltipProducerOverflow.fetch_add(1,std::memory_order_relaxed);
                return result;
            }
            row=&TooltipProducerRows[TooltipProducerRowCount++];
            *row={};row->callerRva=caller;row->tid=GetCurrentThreadId();
            row->outputReadable=TooltipProducerReadHeader(output,row->outputHeader);
            row->sourceReadable=TooltipProducerReadHeader(source,row->sourceHeader);
            row->resultPointer=reinterpret_cast<std::uintptr_t>(result);
        }
        ++row->hits;
        ++row->sampled;
    }
    return result;
}

void ArmTooltipProducer() noexcept {
    if(TooltipProducerInstalled.load(std::memory_order_acquire)) {
        Emit("LOOT_TOOLTIP_PRODUCER_READY version=1.0.0 installed=1 observer-only=1");
        return;
    }
    const auto build=Context?D2RL::GetBuildName(Context):nullptr;
    constexpr std::array<std::uint8_t,18> nativeEntry{{
        0x4C,0x8B,0xDC,0x55,0x53,0x56,0x57,0x49,
        0x8D,0x6B,0xA8,0x48,0x81,0xEC,0x38,0x01,0x00,0x00}};
    // The two direct native callers in the user's build 93847 log.
    constexpr std::array<std::uint8_t,5> ground{{0xE8,0x1F,0x7A,0xF7,0xFF}};
    constexpr std::array<std::uint8_t,5> inventory{{0xE8,0x87,0xBA,0xF6,0xFF}};
    std::array<std::uint8_t,5> groundRead{},inventoryRead{};
    if(!Context || !build || std::string_view(build)!="93847" ||
       !ReadSafe(0x14FC4C,groundRead.data(),groundRead.size()) ||
       !ReadSafe(0x15BBE4,inventoryRead.data(),inventoryRead.size()) ||
       groundRead!=ground || inventoryRead!=inventory ||
       !Context->CheckExpectedBytes(TooltipProducerRva,nativeEntry.data(),
            static_cast<std::uint32_t>(nativeEntry.size()))) {
        Emit("LOOT_TOOLTIP_PRODUCER_REFUSED version=1.0.0 build-or-native-entry-or-callsite-mismatch no-fallback=1");
        return;
    }
    if(!Context->InstallInlineHook(TooltipProducerRva,nativeEntry.data(),
         static_cast<std::uint32_t>(nativeEntry.size()),HookTooltipProducer,
         &OriginalTooltipProducer) || !OriginalTooltipProducer) {
        Emit("LOOT_TOOLTIP_PRODUCER_REFUSED version=1.0.0 loader-hook-install-failed no-fallback=1");
        return;
    }
    TooltipProducerInstalled.store(true,std::memory_order_release);
    Emit("LOOT_TOOLTIP_PRODUCER_READY version=1.0.0 target=D2R+0xC7670 groundReturn=0x14FC51 inventoryReturn=0x15BBE9 ABI=RCX-output,RDX-source,R8/R9-opaque-pass-through return=RAX readOnly=1 opt-in-capture=1 not-confirmed-tooltip-constructor=1");
}

void StartTooltipProducer(std::string_view phase) noexcept {
    if(phase!="idle" && phase!="hidden" && phase!="inventory" && phase!="visible") {
        Emit("LOOT_TOOLTIP_PRODUCER_REFUSED usage: tooltip-producer-start idle|hidden|inventory|visible");return;
    }
    if(!TooltipProducerInstalled.load(std::memory_order_acquire) ||
       !OriginalTooltipProducer) {
        Emit("LOOT_TOOLTIP_PRODUCER_REFUSED install-tooltip-producer-observe-first");return;
    }
    TooltipProducerArmed.store(false,std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(TooltipProducerMutex);
        TooltipProducerRows.fill({});TooltipProducerRowCount=0;
        TooltipProducerCalls.store(0);TooltipProducerSamples.store(0);
        TooltipProducerContended.store(0);TooltipProducerOverflow.store(0);
        for(auto& count:TooltipProducerSiteCalls) count.store(0);
        TooltipProducerOtherCalls.store(0);
        TooltipProducerPhase.fill(0);
        std::memcpy(TooltipProducerPhase.data(),phase.data(),phase.size());
        TooltipProducerDeadline.store(GetTickCount64()+12000,std::memory_order_release);
        TooltipProducerArmed.store(true,std::memory_order_release);
    }
    char message[240]{};
    std::snprintf(message,sizeof(message),
       "LOOT_TOOLTIP_PRODUCER_ARMED version=1.0.0 phase='%s' durationMs=12000 readOnly=1",
       TooltipProducerPhase.data());
    Emit(message);
}

void ReportTooltipProducer(bool stop) noexcept {
    if(stop) TooltipProducerArmed.store(false,std::memory_order_release);
    std::lock_guard<std::mutex> lock(TooltipProducerMutex);
    char message[520]{};
    std::snprintf(message,sizeof(message),
        "LOOT_TOOLTIP_PRODUCER_BEGIN version=1.0.0 phase='%s' installed=%u armed=%u calls=%llu sampled=%llu rows=%zu contended=%llu overflow=%llu no-tooltip-writes=1",
        TooltipProducerPhase.data(),TooltipProducerInstalled.load()?1U:0U,
        TooltipProducerArmed.load()?1U:0U,
        static_cast<unsigned long long>(TooltipProducerCalls.load()),
        static_cast<unsigned long long>(TooltipProducerSamples.load()),
        TooltipProducerRowCount,
        static_cast<unsigned long long>(TooltipProducerContended.load()),
        static_cast<unsigned long long>(TooltipProducerOverflow.load()));
    Emit(message);
    for(std::size_t i=0;i<TooltipProducerReturns.size();++i) {
        std::snprintf(message,sizeof(message),
           "LOOT_TOOLTIP_PRODUCER_CALLS phase='%s' returnRva=D2R+0x%llX allCalls=%llu",
           TooltipProducerPhase.data(),
           static_cast<unsigned long long>(TooltipProducerReturns[i]),
           static_cast<unsigned long long>(TooltipProducerSiteCalls[i].load()));
        Emit(message);
    }
    std::snprintf(message,sizeof(message),
       "LOOT_TOOLTIP_PRODUCER_CALLS phase='%s' returnRva=other allCalls=%llu",
       TooltipProducerPhase.data(),
       static_cast<unsigned long long>(TooltipProducerOtherCalls.load()));
    Emit(message);
    for(std::size_t i=0;i<TooltipProducerRowCount;++i) {
        const auto& r=TooltipProducerRows[i];
        std::snprintf(message,sizeof(message),
            "LOOT_TOOLTIP_PRODUCER_SITE phase='%s' returnRva=D2R+0x%llX hits=%llu tid=%u outReadable=%u outHeader=%08X,%08X,%08X,%08X sourceReadable=%u sourceHeader=%08X,%08X,%08X,%08X result=0x%llX",
            TooltipProducerPhase.data(),
            static_cast<unsigned long long>(r.callerRva),
            static_cast<unsigned long long>(r.hits),r.tid,
            r.outputReadable?1U:0U,r.outputHeader[0],r.outputHeader[1],
            r.outputHeader[2],r.outputHeader[3],
            r.sourceReadable?1U:0U,r.sourceHeader[0],r.sourceHeader[1],
            r.sourceHeader[2],r.sourceHeader[3],
            static_cast<unsigned long long>(r.resultPointer));
        Emit(message);
    }
    Emit("LOOT_TOOLTIP_PRODUCER_END compare-idle-hidden-inventory;no-native-writes;not-a-drop-event");
    FlushCapture();
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
enum class NativeRowPhase : unsigned { Off=0, Hidden=1, Away=2, Inventory=3, Visible=4 };
constexpr std::size_t NativeRowPhaseCount=4;
constexpr std::size_t NativeRowMaxRecords=192;
constexpr std::size_t NativeRowPrintRecords=32;
// Crash-response safety bounds. This shared UI hook still needs live validation:
// take at most eight snapshots per phase, never more often than 180 ms, and
// never run the historical automatic static-code dump concurrently.
constexpr ULONGLONG NativeRowCaptureMs=2'500;
constexpr ULONGLONG NativeRowSampleIntervalMs=180;
constexpr std::uint32_t NativeRowMaxSamplesPerPhase=8;
std::atomic<ULONGLONG> NativeRowNextSampleAt{};
std::atomic<std::uint32_t> NativeRowSamplesTaken{};
std::atomic<NativeRowPhase> NativeRowActivePhase{NativeRowPhase::Off};
std::atomic<ULONGLONG> NativeRowDeadline{};
std::atomic<std::uint64_t> NativeRowCalls{};
std::atomic<std::uint64_t> NativeRowCallerMismatch{};
std::atomic<std::uint64_t> NativeRowLockContention{};
// Deliberate 0.2.0 visual PoC only: Divine Orb, hidden phase, explicit
// command, same-thread latest append + native baseline. Not a production
// renderer-ownership API. The original row is restored before returning.
std::atomic_bool NativeRowBgTrialEnabled{};
std::atomic<std::uint64_t> NativeRowBgRuleGeneration{};
std::atomic<std::uint64_t> NativeRowBgAttempts{},NativeRowBgQualified{};
std::atomic<std::uint64_t> NativeRowBgWrites{},NativeRowBgRestored{};
std::atomic<std::uint64_t> NativeRowBgRejectedChain{},NativeRowBgRejectedCode{};
std::atomic<std::uint64_t> NativeRowBgRejectedRule{},NativeRowBgRejectedColor{};
std::atomic<std::uint64_t> NativeRowBgRejectedBusy{},NativeRowBgRestoreAnomaly{};
std::atomic<std::uint32_t> NativeRowBgLastUnitId{};
std::atomic<std::uint64_t> NativeRowBgLastAppendSeq{};
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
std::atomic_bool NativeRowFontProbeEnabled{};
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
    NativeRowFontProbeEnabled.store(false,std::memory_order_release);
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

constexpr std::size_t NativeRowMaxDistinctUnits=8;
constexpr std::size_t NativeRowLabelMaxBytes=64;
constexpr ULONGLONG NativeRowLabelFreshMs=500;
struct NativeRowLabelEvent final {
    std::uint32_t unitId{},classId{},code{},sourceLength{};
    ULONGLONG tick{};
    std::int64_t qpc{};
    std::uint64_t sequence{};
    DWORD thread{};
    // Read-only SoE V3 call performed synchronously INSIDE the V1 callback.
    // 0=no V3 API, 1=scope-empty, 2=scope-active. Never retain nativeUnit.
    std::uint8_t builderScopeState{};
    bool builderScopeMatches{};
    std::uint32_t builderScopeUnitId{};
    std::array<std::uint8_t,NativeRowLabelMaxBytes> source{};
};
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
struct NativeRowLabelSnapshot final {
    NativeRowLabelEvent latest{};
    std::uint32_t distinctUnitIds{};
    std::uint64_t callbacks{};
    bool captured{};
};
struct NativeRowLabelPhase final {
    std::mutex mutex{};
    std::array<NativeRowLabelEvent,NativeRowMaxDistinctUnits> units{};
    std::size_t uniqueCount{};
    std::uint64_t callbacks{},overflow{},withoutText{},nextSequence{};
    std::uint64_t builderScopedMatches{},builderScopedMisses{},builderNoScope{};
    NativeRowLabelEvent latest{};
};
std::array<NativeRowLabelPhase,NativeRowPhaseCount> NativeRowLabelPhases{};
std::atomic<std::uint64_t> NativeRowLabelEventContention{};
std::atomic<std::uint64_t> NativeRowLabelSnapshotContention{};

// Fail-closed source-event witness. Standalone backend has no borrowed source
// text, which is logged as unavailable rather than fabricated. Only collect
// while a manual 2.5-second phase is actually active. No hook I/O or blocking.
void ObserveNativeRowLabelEvent(std::int32_t type,
    std::uint32_t classId,std::uint32_t unitId,std::uint32_t rawCode,
    const char* source,std::uint32_t sourceLength) noexcept {
    const auto phase=NativeRowActivePhase.load(std::memory_order_acquire);
    if (phase==NativeRowPhase::Off || type!=4 || !unitId || sourceLength>255)
        return;
    const auto now=GetTickCount64();
    if (now>=NativeRowDeadline.load(std::memory_order_acquire)) return;
    auto& bucket=NativeRowLabelPhases[static_cast<std::size_t>(phase)-1U];
    if (!bucket.mutex.try_lock()) {
        NativeRowLabelEventContention.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    std::unique_lock<std::mutex> lock(bucket.mutex,std::adopt_lock);
    if (NativeRowActivePhase.load(std::memory_order_acquire)!=phase ||
        now>=NativeRowDeadline.load(std::memory_order_acquire)) return;
    NativeRowLabelEvent event{};
    event.unitId=unitId;
    event.classId=classId;
    event.code=CanonicalItemCode(rawCode);
    event.thread=GetCurrentThreadId();
    event.tick=now;
    LARGE_INTEGER eventQpc{};
    if (QueryPerformanceCounter(&eventQpc)) event.qpc=eventQpc.QuadPart;
    event.sequence=++bucket.nextSequence;
    if (const auto* scope=InWorldRenderScopeApi.load(std::memory_order_acquire);
        scope && scope->getCurrentItem) {
        SoE::Interop::InWorldActiveItemV3 active{};
        active.structSize=sizeof(active);
        if (scope->getCurrentItem(&active)) {
            event.builderScopeState=2;
            event.builderScopeUnitId=active.unitId;
            event.builderScopeMatches=active.unitType==type &&
                active.classId==classId && active.unitId==unitId;
            if (event.builderScopeMatches) ++bucket.builderScopedMatches;
            else ++bucket.builderScopedMisses;
        } else {event.builderScopeState=1;++bucket.builderNoScope;}
    }
    if (source && sourceLength) {
        event.sourceLength=sourceLength;
        std::memcpy(event.source.data(),source,
            std::min<std::size_t>(event.sourceLength,event.source.size()));
    } else ++bucket.withoutText;
    ++bucket.callbacks;
    bucket.latest=event;
    NativeRowLabelEvent* unit=nullptr;
    for(std::size_t i=0;i<bucket.uniqueCount;++i) {
        if (bucket.units[i].unitId==unitId && bucket.units[i].code==event.code) {
            unit=&bucket.units[i];break;
        }
    }
    if(!unit && bucket.uniqueCount<bucket.units.size())
        unit=&bucket.units[bucket.uniqueCount++];
    if(unit) *unit=event;
    else ++bucket.overflow;
}

// Live fast path observes every SoE V1 item callback, independently from the
// diagnostic's 256-append/8-render window. Only immutable bytes are retained.
void ObserveNativeRowLiveLabel(std::int32_t type,
    std::uint32_t classId,std::uint32_t unitId,std::uint32_t rawCode,
    const void* nativeUnit,const char* source,std::uint32_t sourceLength) noexcept {
    if (!NativeRowBgLiveEnabled.load(std::memory_order_acquire) &&
        !NativeRowFontProbeEnabled.load(std::memory_order_acquire)) return;
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

NativeRowLabelSnapshot ReadNativeRowLabelSnapshot(NativeRowPhase phase) noexcept {
    NativeRowLabelSnapshot out{};
    if (phase==NativeRowPhase::Off) return out;
    auto& bucket=NativeRowLabelPhases[static_cast<std::size_t>(phase)-1U];
    if (!bucket.mutex.try_lock()) {
        NativeRowLabelSnapshotContention.fetch_add(1,std::memory_order_relaxed);
        return out;
    }
    std::unique_lock<std::mutex> lock(bucket.mutex,std::adopt_lock);
    out.latest=bucket.latest;
    out.distinctUnitIds=static_cast<std::uint32_t>(bucket.uniqueCount);
    out.callbacks=bucket.callbacks;
    out.captured=true;
    return out;
}

struct NativeRowRecord final {
    std::uintptr_t address{};
    std::array<std::int32_t,4> firstRect{};
    std::array<std::int32_t,4> lastRect{};
    std::array<std::uint32_t,4> firstColorBits{};
    std::array<std::uint32_t,4> lastColorBits{};
    std::array<std::uint8_t,0x50> firstTextHeaders{},lastTextHeaders{};
    std::array<NativeRowTextWitness,2> firstText{},lastText{};
    std::uint64_t calls{};
    std::uint32_t textHeaderTransitions{};
    std::uint32_t colorTransitions{};
    std::uint32_t geometryTransitions{};
    DWORD thread{};
    ULONGLONG firstTick{},lastTick{};
    NativeRowLabelSnapshot firstLabel{},lastLabel{};
};
// Eight per-invocation snapshots, not just first/last of the reusable slot.
// Carries copied event values, copied row text and diagnostics, no borrowed
// pointers and no native UI writes. V3 is tested at THIS renderer invocation.
struct NativeRowChainSample final {
    std::uintptr_t element{};
    std::uint32_t order{};
    ULONGLONG tick{};
    std::int64_t qpc{};
    DWORD thread{};
    std::array<std::int32_t,4> rect{};
    std::array<std::uint32_t,4> colorBits{};
    NativeRowTextWitness text{};
    NativeRowLabelSnapshot event{};
    std::uint8_t rendererScopeState{};
    std::uint32_t rendererScopeUnitId{};
    bool rendererScopeMatchesEvent{};
    // Read-only full-stride candidate scan: 0=not attempted, 1=readable,
    // 2=inaccessible, 3=no candidate item ID. Never authorizes a native write.
    std::uint8_t handoffReadState{};
    std::uint32_t previousEventUnitId{};
    std::uint64_t appendFence{},rendererMatchedAppendSeq{};
    std::uintptr_t rendererComponent{},rendererQueueData{};
    std::uint64_t rendererQueueCount{};
    // 0=not observed,1=live component+0x168 agrees with last append,
    // 2=descriptor mismatch,3=unreadable,4=no preceding append,5=busy.
    std::uint8_t rendererVectorState{};
    NativeRowHandoffScan::Matches candidateIdFields{};
    NativeRowHandoffScan::Matches previousIdFields{};
};
struct NativeRowBucket final {
    std::mutex mutex{};
    std::array<NativeRowChainSample,NativeRowMaxSamplesPerPhase> chain{};
    std::size_t chainSize{};
    std::uint32_t lastCapturedEventUnitId{};
    std::array<NativeRowRecord,NativeRowMaxRecords> records{};
    std::size_t size{};
    std::uint64_t calls{},readFailures{},contention{},overflow{};
    std::uint64_t invalidColor{},otherCallers{};
};
// Capture only the append's pre/post 0x18-byte vector descriptor and its
// freshly constructed row. No adjacent slot traversal, heap scanning, or
// reliance on item text for binding. All pointers are per-phase diagnostics.
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
struct NativeRowAppendEvent final {
    std::uint64_t sequence{};
    std::uintptr_t component{},data{},row{};
    std::uint64_t beforeCount{},afterCount{},capacity{};
    std::int64_t qpc{};
    DWORD thread{};
    NativeRowLabelSnapshot label{};
    NativeRowTextWitness text{};
    std::uint32_t appendScopeUnitId{};
    std::uint8_t appendScopeState{}; // 0=API absent, 1=empty, 2=active
    bool vectorValid{},rowReadable{};
};
struct NativeRowAppendBucket final {
    std::mutex mutex{};
    std::array<NativeRowAppendEvent,NativeRowAppendMaxEvents> events{};
    std::size_t size{};
    std::uint64_t qualified{},inaccessible{},unexpectedCount{},
        unreadableRow{},contended{},overflow{};
};
std::array<NativeRowAppendBucket,NativeRowPhaseCount> NativeRowAppendBuckets{};
std::atomic<std::uint64_t> NativeRowAppendCallerMismatch{};
std::atomic<std::uint64_t> NativeRowAppendContention{};
std::array<std::atomic<std::uint64_t>,NativeRowPhaseCount>
    NativeRowAppendContentionByPhase{};
std::array<std::atomic<std::uint32_t>,NativeRowPhaseCount>
    NativeRowAppendAttemptsByPhase{};
bool ReadNativeStyledVector(void*,NativeStyledTextVector&) noexcept;
void __fastcall HookNativeRowAppend(void*,const void*,const void*,
    const void*,const void*) noexcept;
std::array<NativeRowBucket,NativeRowPhaseCount> NativeRowBuckets{};
constexpr std::array<const char*,NativeRowPhaseCount> NativeRowPhaseNames{{
    "hidden","away","inventory","visible"
}};

// Bounded, opt-in, read-only text candidate. Do not chase pointers until
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

std::array<NativeRowTextWitness,2> InspectNativeRowTextHeaders(
    const std::array<std::uint8_t,0x50>& header,
    std::uintptr_t elementAddress) noexcept {
    return {{ InspectNativeRowTextCandidate(header,0x00,elementAddress),
              InspectNativeRowTextCandidate(header,0x28,elementAddress) }};
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

// Second, optional bounded native row copy, only after the 0x178-byte
// existing snapshot has succeeded and ONLY for one of the eight samples.
// The render-element stride comes from validated 93847 caller instructions.
// No pointer chasing or arbitrary memory/heap scan: exactly one row, one page
// region, one synchronous read before calling the original renderer.
bool ReadNativeRowHandoffBytes(void* element,
    std::array<std::uint8_t,NativeRowHandoffScan::RowStride>& out) noexcept {
    if (!element) return false;
    const auto address=reinterpret_cast<std::uintptr_t>(element);
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(element,&region,sizeof(region)) ||
        region.State!=MEM_COMMIT ||
        (region.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
    const auto protection=region.Protect&0xFFU;
    const bool readable=protection==PAGE_READONLY ||
        protection==PAGE_READWRITE || protection==PAGE_WRITECOPY ||
        protection==PAGE_EXECUTE_READ ||
        protection==PAGE_EXECUTE_READWRITE ||
        protection==PAGE_EXECUTE_WRITECOPY;
    if (!readable) return false;
    const auto start=reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (address<start || out.size()>region.RegionSize ||
        address-start>region.RegionSize-out.size()) return false;
    std::memcpy(out.data(),element,out.size());
    return true;
}

// Keep native renderer detour shallow (MSVC C1061 regression avoidance).
void PopulateNativeRowHandoffEvidence(void* element,
    NativeRowChainSample& trace) noexcept {
    if (!trace.event.latest.unitId) {
        trace.handoffReadState=3;
        return;
    }
    std::array<std::uint8_t,NativeRowHandoffScan::RowStride> rowBytes{};
    if (!ReadNativeRowHandoffBytes(element,rowBytes)) {
        trace.handoffReadState=2;
        return;
    }
    trace.handoffReadState=1;
    trace.candidateIdFields=NativeRowHandoffScan::FindAlignedId(
        rowBytes.data(),rowBytes.size(),trace.event.latest.unitId);
    if (trace.previousEventUnitId &&
        trace.previousEventUnitId!=trace.event.latest.unitId)
        trace.previousIdFields=NativeRowHandoffScan::FindAlignedId(
            rowBytes.data(),rowBytes.size(),trace.previousEventUnitId);
}

// Read-only invocation-time witness: match only a COMPLETED, PRECEDING
// append on the current thread and the exact row address. The renderer does
// not expose a component argument; this confirms the selected append's
// component STILL owns this vector at render time, not that D2R passes the
// item pointer into the renderer. Zero stack-trace or UI-global speculation.
void WitnessRendererAppendVector(std::size_t index,
    NativeRowChainSample& trace) noexcept {
    const auto& live=NativeRowAppendBuckets[index];
    auto& bucket=const_cast<NativeRowAppendBucket&>(live);
    if (!bucket.mutex.try_lock()) {
        trace.rendererVectorState=5;
        NativeRowAppendContention.fetch_add(1,std::memory_order_relaxed);
        NativeRowAppendContentionByPhase[index].fetch_add(
            1,std::memory_order_relaxed);
        return;
    }
    std::uintptr_t component{},data{};
    std::uint64_t count{},beforeCount{},sequence{};
    std::int64_t latestQpc{};
    for(std::size_t i=0;i<bucket.size;++i) {
        const auto& e=bucket.events[i];
        if (!e.vectorValid || !e.sequence || e.sequence>trace.appendFence ||
            e.row!=trace.element || e.thread!=trace.thread ||
            !NativeRowLatestMatch::Precedes(e.qpc,trace.qpc)) continue;
        if (NativeRowLatestMatch::Later(e.sequence,e.qpc,
                sequence,latestQpc)) {
            component=e.component;data=e.data;
            count=e.afterCount;beforeCount=e.beforeCount;
            sequence=e.sequence;latestQpc=e.qpc;
        }
    }
    bucket.mutex.unlock();
    if (!sequence) {trace.rendererVectorState=4;return;}
    trace.rendererMatchedAppendSeq=sequence;
    trace.rendererComponent=component;
    NativeStyledTextVector current{};
    if (!ReadNativeStyledVector(reinterpret_cast<void*>(component),current)) {
        trace.rendererVectorState=3;return;
    }
    trace.rendererQueueData=current.data;
    trace.rendererQueueCount=current.count;
    trace.rendererVectorState=
        current.data==data && current.count==count &&
        NativeRowAppendMatch::ValidNewRow(current.data,beforeCount,
            current.count,current.capacity) &&
        current.data+(count-1)*NativeRowHandoffScan::RowStride==trace.element
        ? 1 : 2;
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



void ResetNativeRowRuntime() noexcept {
    NativeRowBgTrialEnabled.store(false,std::memory_order_release);
    NativeRowActivePhase.store(NativeRowPhase::Off,std::memory_order_release);
    NativeRowDeadline.store(0,std::memory_order_release);
    NativeRowNextSampleAt.store(0,std::memory_order_release);
    NativeRowSamplesTaken.store(0,std::memory_order_release);
    for (auto& bucket:NativeRowBuckets) {
        std::lock_guard lock(bucket.mutex);
        bucket.records.fill({});bucket.size=0;
        bucket.chain.fill({});bucket.chainSize=0;
        bucket.lastCapturedEventUnitId=0;
        bucket.calls=0;bucket.readFailures=0;
        bucket.contention=0;bucket.overflow=0;
        bucket.invalidColor=0;bucket.otherCallers=0;
    }
    NativeRowAppendsTaken.store(0,std::memory_order_release);
    for (auto& c:NativeRowAppendContentionByPhase)
        c.store(0,std::memory_order_release);
    for (auto& attempts:NativeRowAppendAttemptsByPhase)
        attempts.store(0,std::memory_order_release);
    NativeRowAppendSequence.store(0,std::memory_order_release);
    NativeRowAppendCommittedSequence.store(0,std::memory_order_release);
    for (auto& append:NativeRowAppendBuckets) {
        std::lock_guard lock(append.mutex);
        append.events.fill({});append.size=0;
        append.qualified=append.inaccessible=append.unexpectedCount=0;
        append.unreadableRow=append.contended=append.overflow=0;
    }
    for (auto& label:NativeRowLabelPhases) {
        std::lock_guard lock(label.mutex);
        label.units.fill({});label.uniqueCount=0;
        label.callbacks=0;label.overflow=0;label.withoutText=0;
        label.nextSequence=0;label.builderScopedMatches=0;
        label.builderScopedMisses=0;label.builderNoScope=0;
        label.latest={};
    }
}

void StartNativeRowPhase(std::string_view name,bool bgTrial=false) noexcept {
    if (!NativeRowRendererHookInstalled.load(std::memory_order_acquire) ||
        !NativeRowAppendHookInstalled.load(std::memory_order_acquire)) {
        Emit("LOOT_NATIVE_ROW_PHASE_REFUSED reason=observer-not-installed use-native-row-arm-first");
        return;
    }
    NativeRowPhase phase=NativeRowPhase::Off;
    for (std::size_t i=0;i<NativeRowPhaseCount;++i)
        if(name==NativeRowPhaseNames[i])
            phase=static_cast<NativeRowPhase>(i+1U);
    if (phase==NativeRowPhase::Off) {
        Emit("LOOT_NATIVE_ROW_PHASE_REFUSED usage=native-row-start hidden|away|inventory|visible");
        return;
    }
    NativeRowBgTrialEnabled.store(false,std::memory_order_release);
    NativeRowActivePhase.store(NativeRowPhase::Off,std::memory_order_release);
    auto& bucket=NativeRowBuckets[static_cast<std::size_t>(phase)-1U];
    {
        std::lock_guard lock(bucket.mutex);
        bucket.records.fill({});bucket.size=0;
        bucket.chain.fill({});bucket.chainSize=0;
        bucket.lastCapturedEventUnitId=0;
        bucket.calls=0;bucket.readFailures=0;
        bucket.contention=0;bucket.overflow=0;
        bucket.invalidColor=0;bucket.otherCallers=0;
    }
    {
        auto& append=NativeRowAppendBuckets[static_cast<std::size_t>(phase)-1U];
        std::lock_guard lock(append.mutex);
        append.events.fill({});append.size=0;
        append.qualified=append.inaccessible=append.unexpectedCount=0;
        append.unreadableRow=append.contended=append.overflow=0;
    }
    {
        auto& label=NativeRowLabelPhases[static_cast<std::size_t>(phase)-1U];
        std::lock_guard lock(label.mutex);
        label.units.fill({});label.uniqueCount=0;
        label.callbacks=0;label.overflow=0;label.withoutText=0;
        label.nextSequence=0;label.builderScopedMatches=0;
        label.builderScopedMisses=0;label.builderNoScope=0;
        label.latest={};
    }
    NativeRowAppendsTaken.store(0,std::memory_order_release);
    NativeRowAppendContentionByPhase[static_cast<std::size_t>(phase)-1U]
        .store(0,std::memory_order_release);
    NativeRowAppendAttemptsByPhase[static_cast<std::size_t>(phase)-1U]
        .store(0,std::memory_order_release);
    NativeRowAppendSequence.store(0,std::memory_order_release);
    NativeRowAppendCommittedSequence.store(0,std::memory_order_release);
    NativeRowSamplesTaken.store(0,std::memory_order_release);
    NativeRowNextSampleAt.store(0,std::memory_order_release);
    NativeRowDeadline.store(GetTickCount64()+NativeRowCaptureMs,
        std::memory_order_release);
    NativeRowActivePhase.store(phase,std::memory_order_release);
    if (bgTrial && phase==NativeRowPhase::Hidden)
        NativeRowBgTrialEnabled.store(true,std::memory_order_release);
    char line[310]{};
    std::snprintf(line,sizeof(line),
        "LOOT_NATIVE_ROW_PHASE_BEGIN version=1.0.0 phase=%.*s durationMs=%llu maxSamples=8 minSampleGapMs=180 autoHoverStaticDumps=0 firstItemId=%u itemIdentity=temporal-only nativeWrites=%s no-hook-io=1",
        static_cast<int>(name.size()),name.data(),
        static_cast<unsigned long long>(NativeRowCaptureMs),
        HoverDiffItemId.load(std::memory_order_acquire),
        bgTrial?"temporary-float4":"0");
    Emit(line);FlushCapture();
}

// Text equality + fresh same-thread V1 event is a useful temporal witness,
// NOT a verified native render-element -> UnitAny ownership relation.
NativeRowLabelCorrelation::Relation CompareNativeRowItemEvent(
    const NativeRowTextWitness& rowText,
    const NativeRowLabelEvent& event) noexcept {
    if (rowText.size==0 || rowText.size>rowText.raw.size() ||
        rowText.captured!=rowText.size || event.sourceLength==0 ||
        event.sourceLength>event.source.size())
        return NativeRowLabelCorrelation::Relation::Unavailable;
    return NativeRowLabelCorrelation::Compare(
        {reinterpret_cast<const char*>(rowText.raw.data()),
            static_cast<std::size_t>(rowText.size)},
        {reinterpret_cast<const char*>(event.source.data()),
            static_cast<std::size_t>(event.sourceLength)});
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

// Restricted native-color experiment. Read the current row and the latest
// completed append within THIS invocation; never use a historical pointer as
// authority. No background modification when scope/event/sequence/vector or
// exact original text disagree. Live rows are not retained across frames.
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
    if (!exact || !NativeRowBgTrialPolicy::VanillaHiddenBlack(nativeColor)) {
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
            NativeRowBgTrialEnabled.load(std::memory_order_acquire) ||
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
        !NativeRowBgTrialEnabled.load(std::memory_order_acquire) &&
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
    TraceLootLatency(append.code,append.unitId,LootLatencyStage::HoverPaint,
        "native-hover-row-bg-forwarded");
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
        !NativeRowBgTrialPolicy::SingleRow(before.count,after.count)) return;
    candidate.data=after.data;
    candidate.row=after.data;
    std::array<std::int32_t,4> rect{};
    std::array<std::uint32_t,4> color{};
    std::array<std::uint8_t,0x50> header{};
    if (!SnapshotNativeRow(reinterpret_cast<void*>(candidate.row),
            rect,color,header) ||
        !NativeRowBgTrialPolicy::VanillaHiddenBlack(color)) return;
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

// A strict, explanatory report: identical row pointers and copied display
// text establish append->render *record* reuse, not item unit ownership.
void ReportNativeRowAppendMatch(std::size_t phaseIndex,
    const std::array<NativeRowChainSample,NativeRowMaxSamplesPerPhase>& chain,
    std::size_t chainSize) noexcept {
    const char* phase=NativeRowPhaseNames[phaseIndex];
    NativeRowAppendBucket& live=NativeRowAppendBuckets[phaseIndex];
    std::array<NativeRowAppendEvent,NativeRowAppendMaxEvents> events{};
    std::size_t count{};
    std::uint64_t qualified{},inaccessible{},badCount{},unreadable{},overflow{};
    {
        std::lock_guard lock(live.mutex);
        count=live.size;
        std::copy_n(live.events.begin(),count,events.begin());
        qualified=live.qualified;inaccessible=live.inaccessible;
        badCount=live.unexpectedCount;unreadable=live.unreadableRow;
        overflow=live.overflow;
    }
    LARGE_INTEGER qpf{};
    const bool frequencyOk=QueryPerformanceFrequency(&qpf) && qpf.QuadPart>0;
    unsigned exact{},ambiguous{},unmatched{},textMatches{},sameEvent{};
    unsigned appendScoped{};
    for(std::size_t j=0;j<count;++j)
        if(events[j].appendScopeState==2) ++appendScoped;
    char line[960]{};
    for(std::size_t i=0;i<chainSize;++i) {
        const auto& render=chain[i];
        const NativeRowAppendEvent* nearest{};
        std::size_t matches=0;
        for(std::size_t j=0;j<count;++j) {
            const auto& append=events[j];
            if (!append.vectorValid || !frequencyOk ||
                !NativeRowAppendMatch::RecentSameRow(
                    append.row,render.element,append.thread,render.thread,
                    append.qpc,render.qpc,qpf.QuadPart)) continue;
            ++matches;
            if (!nearest || append.qpc>nearest->qpc) nearest=&append;
        }
        if (!nearest) ++unmatched;
        else if (matches!=1) ++ambiguous;
        else ++exact;
        NativeRowLabelCorrelation::Relation relation=
            NativeRowLabelCorrelation::Relation::Unavailable;
        if (nearest && nearest->rowReadable &&
            nearest->text.size && render.text.size &&
            nearest->text.captured==nearest->text.size &&
            render.text.captured==render.text.size)
            relation=NativeRowLabelCorrelation::Compare(
                {reinterpret_cast<const char*>(nearest->text.raw.data()),
                    static_cast<std::size_t>(nearest->text.size)},
                {reinterpret_cast<const char*>(render.text.raw.data()),
                    static_cast<std::size_t>(render.text.size)});
        if (relation==NativeRowLabelCorrelation::Relation::Exact)
            ++textMatches;
        const bool eventMatches=nearest && nearest->label.latest.unitId &&
            render.event.latest.unitId==nearest->label.latest.unitId &&
            render.event.latest.sequence==nearest->label.latest.sequence;
        if (eventMatches) ++sameEvent;
        const auto ageUs=nearest && frequencyOk ?
            static_cast<unsigned long long>(
                (render.qpc-nearest->qpc)*1000000LL/qpf.QuadPart):0ULL;
        std::snprintf(line,sizeof(line),
            "LOOT_NATIVE_ROW_APPEND_MATCH version=1.0.0 phase=%s render=%zu "
            "row=0x%llX candidateAppends=%zu component=0x%llX "
            "queueData=0x%llX queueIndex=%llu queuedCount=%llu "
            "ageUs=%llu renderUnitId=%u appendEventUnitId=%u "
            "sameLabelSequence=%u textRelation=%s appendScopeState=%u "
            "appendScopeUnitId=%u "
            "conclusion=record-correlation-only-item-ownership-NOT-PROVEN writes=0",
            phase,i,static_cast<unsigned long long>(render.element),matches,
            static_cast<unsigned long long>(nearest?nearest->component:0),
            static_cast<unsigned long long>(nearest?nearest->data:0),
            static_cast<unsigned long long>(nearest?nearest->afterCount-1:0),
            static_cast<unsigned long long>(nearest?nearest->afterCount:0),
            ageUs,render.event.latest.unitId,
            nearest?nearest->label.latest.unitId:0U,eventMatches?1U:0U,
            NativeRowLabelCorrelation::Name(relation),
            nearest?unsigned(nearest->appendScopeState):0U,
            nearest?nearest->appendScopeUnitId:0U);
        Emit(line);
    }
    std::snprintf(line,sizeof(line),
        "LOOT_NATIVE_ROW_APPEND_SUMMARY version=1.0.0 phase=%s "
        "recorded=%zu qualified=%llu inaccessible=%llu unexpectedCount=%llu "
        "unreadableRow=%llu overflow=%llu contentionGlobal=%llu "
        "appendHookInstalled=%u renderSamples=%zu uniquePointerMatches=%u "
        "ambiguousPointerMatches=%u unmatched=%u textMatches=%u "
        "sameLabelSequence=%u appendScopeActive=%u "
        "itemOwnership=NOT-PROVEN hiddenHoverBackgroundWrites=0",
        phase,count,static_cast<unsigned long long>(qualified),
        static_cast<unsigned long long>(inaccessible),
        static_cast<unsigned long long>(badCount),
        static_cast<unsigned long long>(unreadable),
        static_cast<unsigned long long>(overflow),
        static_cast<unsigned long long>(NativeRowAppendContention.load()),
        NativeRowAppendHookInstalled.load()?1U:0U,
        chainSize,exact,ambiguous,unmatched,textMatches,sameEvent,
        appendScoped);
    Emit(line);
}

// 0.1.99: deterministic candidate selection by a completed append sequence,
// not by number of historical appearances of a reused row pointer. A pass
// still does NOT prove game-item ownership; it supports a narrower next probe.
void ReportNativeRowLatestAppend(std::size_t phaseIndex,
    const std::array<NativeRowChainSample,NativeRowMaxSamplesPerPhase>& chain,
    std::size_t chainSize) noexcept {
    const char* phase=NativeRowPhaseNames[phaseIndex];
    auto& live=NativeRowAppendBuckets[phaseIndex];
    std::array<NativeRowAppendEvent,NativeRowAppendMaxEvents> events{};
    std::size_t count{};
    std::uint64_t invalid{},overflow{},qualified{};
    {
        std::lock_guard lock(live.mutex);
        count=live.size;
        std::copy_n(live.events.begin(),count,events.begin());
        invalid=live.inaccessible+live.unexpectedCount+live.unreadableRow;
        overflow=live.overflow;
        qualified=live.qualified;
    }
    const auto contention=NativeRowAppendContentionByPhase[phaseIndex].load(
        std::memory_order_acquire);
    const bool gapFree=invalid==0 && overflow==0 && contention==0 &&
        qualified==count &&
        NativeRowAppendAttemptsByPhase[phaseIndex].load(
            std::memory_order_acquire)<=NativeRowAppendMaxEvents;
    LARGE_INTEGER qpf{};
    const bool frequencyOk=QueryPerformanceFrequency(&qpf) &&
        qpf.QuadPart>=20;
    unsigned resolved{},rejected{},missing{},vectorOk{},reuse{};
    char line[1024]{};
    for(std::size_t i=0;i<chainSize;++i) {
        const auto& r=chain[i];
        const NativeRowAppendEvent* selected{};
        std::size_t candidates{};
        bool sequenceCollision=false;
        for(std::size_t j=0;j<count;++j) {
            const auto& e=events[j];
            if (!e.vectorValid || !e.rowReadable ||
                e.thread!=r.thread || e.row!=r.element ||
                !NativeRowLatestMatch::FenceAllows(e.sequence,r.appendFence) ||
                !NativeRowAppendMatch::RecentSameRow(e.row,r.element,
                    e.thread,r.thread,e.qpc,r.qpc,
                    frequencyOk?qpf.QuadPart:0)) continue;
            ++candidates;
            if (!selected || NativeRowLatestMatch::Later(e.sequence,e.qpc,
                    selected->sequence,selected->qpc)) selected=&e;
            else if (e.sequence==selected->sequence) sequenceCollision=true;
        }
        if(candidates>1) ++reuse;
        NativeRowLabelCorrelation::Relation relation=
            NativeRowLabelCorrelation::Relation::Unavailable;
        if (selected && selected->text.size && r.text.size &&
            selected->text.captured==selected->text.size &&
            r.text.captured==r.text.size)
            relation=NativeRowLabelCorrelation::Compare(
                {reinterpret_cast<const char*>(selected->text.raw.data()),
                    static_cast<std::size_t>(selected->text.size)},
                {reinterpret_cast<const char*>(r.text.raw.data()),
                    static_cast<std::size_t>(r.text.size)});
        const bool eventSame=selected &&
            selected->label.latest.sequence!=0 &&
            selected->label.latest.sequence==r.event.latest.sequence &&
            selected->label.latest.unitId==r.event.latest.unitId;
        const bool scopeSame=selected && selected->appendScopeState==2 &&
            selected->appendScopeUnitId!=0 &&
            selected->appendScopeUnitId==selected->label.latest.unitId;
        const bool vectorSame=selected && r.rendererVectorState==1 &&
            r.rendererMatchedAppendSeq==selected->sequence &&
            r.rendererComponent==selected->component &&
            r.rendererQueueData==selected->data &&
            r.rendererQueueCount==selected->afterCount;
        if(vectorSame)++vectorOk;
        const bool pass=gapFree && frequencyOk && selected &&
            !sequenceCollision && eventSame && scopeSame && vectorSame &&
            relation==NativeRowLabelCorrelation::Relation::Exact;
        if(pass)++resolved;
        else if(!selected)++missing;
        else ++rejected;
        const auto ageUs=selected && frequencyOk ?
            static_cast<unsigned long long>(
                (r.qpc-selected->qpc)*1000000LL/qpf.QuadPart):0ULL;
        std::snprintf(line,sizeof(line),
            "LOOT_NATIVE_ROW_LATEST_APPEND version=1.0.0 phase=%s render=%zu "
            "appendFence=%llu selectedSeq=%llu candidateHistory=%zu "
            "component=0x%llX row=0x%llX queueIndex=%llu "
            "appendUnitId=%u renderEventUnitId=%u labelSeq=%llu "
            "eventSame=%u scopeSame=%u rendererVectorState=%u "
            "rendererVectorSame=%u textRelation=%s ageUs=%llu "
            "gapFree=%u deterministicRecord=%u gameItemOwnership=UNPROVEN "
            "backgroundWrites=0",
            phase,i,
            static_cast<unsigned long long>(r.appendFence),
            static_cast<unsigned long long>(selected?selected->sequence:0),
            candidates,
            static_cast<unsigned long long>(selected?selected->component:0),
            static_cast<unsigned long long>(r.element),
            static_cast<unsigned long long>(selected?selected->afterCount-1:0),
            selected?selected->appendScopeUnitId:0U,r.event.latest.unitId,
            static_cast<unsigned long long>(selected?
                selected->label.latest.sequence:0),
            eventSame?1U:0U,scopeSame?1U:0U,unsigned(r.rendererVectorState),
            vectorSame?1U:0U,NativeRowLabelCorrelation::Name(relation),
            ageUs,gapFree?1U:0U,pass?1U:0U);
        Emit(line);
    }
    std::snprintf(line,sizeof(line),
        "LOOT_NATIVE_ROW_LATEST_SUMMARY version=1.0.0 phase=%s "
        "renderSamples=%zu deterministicRecords=%u rejected=%u missing=%u "
        "reusedPointerHistories=%u rendererVectorMatches=%u "
        "qualifiedAppends=%llu retained=%zu invalid=%llu overflow=%llu "
        "contention=%llu gapFree=%u itemOwnership=UNPROVEN "
        "nativeBackgroundWrites=0",
        phase,chainSize,resolved,rejected,missing,reuse,vectorOk,
        static_cast<unsigned long long>(qualified),count,
        static_cast<unsigned long long>(invalid),
        static_cast<unsigned long long>(overflow),
        static_cast<unsigned long long>(contention),gapFree?1U:0U);
    Emit(line);
}

void ReportNativeRowItemEvent(const char* phase,std::size_t ordinal,
    std::uintptr_t element,const char* position,
    const NativeRowTextWitness& rowText,
    const NativeRowLabelSnapshot& label,
    ULONGLONG sampleTick,DWORD sampleThread,
    std::uint32_t duplicateSourceUnitIds,
    bool identityOverflow) noexcept {
    const auto& e=label.latest;
    const bool temporal=label.captured && e.unitId!=0 &&
        sampleTick>=e.tick;
    const auto age=temporal?sampleTick-e.tick:0;
    const bool fresh=temporal && age<=NativeRowLabelFreshMs;
    const bool sameThread=temporal && e.thread==sampleThread;
    const auto relation=CompareNativeRowItemEvent(rowText,e);
    char code[5]{char(e.code&0xFF),char((e.code>>8)&0xFF),
        char((e.code>>16)&0xFF),char((e.code>>24)&0xFF),0};
    char sourcePreview[65]{};
    const auto length=std::min<std::size_t>(e.sourceLength,
        e.source.size());
    for(std::size_t i=0;i<length;++i)
        sourcePreview[i]=e.source[i]>=0x20 && e.source[i]<=0x7E
            ?static_cast<char>(e.source[i]):'.';
    char line[800]{};
    std::snprintf(line,sizeof(line),
        "LOOT_NATIVE_ROW_ITEM_EVENT version=1.0.0 phase=%s ordinal=%zu position=%s element=0x%llX "
        "eventCaptured=%u eventUnitId=%u eventClassId=%u eventCode='%.4s' "
        "eventSourceLength=%u eventSourcePreview='%s' labelCallbacksAtSample=%llu "
        "unitIdsAtSample=%u eventAgeMs=%llu ageValid=%u fresh500ms=%u "
        "sameThread=%u textRelation=%s duplicateSourceUnitIds=%u "
        "unitListOverflow=%u conclusion=label-correlation-only-item-ownership-NOT-PROVEN nativeWrites=0",
        phase,ordinal,position,static_cast<unsigned long long>(element),
        label.captured?1U:0U,e.unitId,e.classId,code,
        e.sourceLength,sourcePreview,
        static_cast<unsigned long long>(label.callbacks),
        label.distinctUnitIds,static_cast<unsigned long long>(age),
        temporal?1U:0U,fresh?1U:0U,sameThread?1U:0U,
        NativeRowLabelCorrelation::Name(relation),
        duplicateSourceUnitIds,identityOverflow?1U:0U);
    Emit(line);
}

// 0.1.99: disambiguation probe for the actual 93847 render-row payload.
// For each immutable sampled element, only report candidate aligned ID fields;
// never treat absence or a coincidental integer match as an ownership proof.
void ReportNativeRowHandoffSamples(const char* phase,
    const std::array<NativeRowChainSample,NativeRowMaxSamplesPerPhase>& chain,
    std::size_t count) noexcept {
    std::uint32_t readable{},unreadable{},noEventId{},hits{},transitions{};
    char line[620]{};
    for(std::size_t i=0;i<count;++i) {
        const auto& row=chain[i];
        const auto id=row.event.latest.unitId;
        if (row.handoffReadState==1) ++readable;
        else if(row.handoffReadState==2) ++unreadable;
        else if(row.handoffReadState==3) ++noEventId;
        if(row.candidateIdFields.total) ++hits;
        if(row.previousEventUnitId && id &&
            row.previousEventUnitId!=id) ++transitions;
        constexpr unsigned missing=0xFFFF;
        const auto first=row.candidateIdFields.total ?
            unsigned(row.candidateIdFields.offsets[0]) : missing;
        const auto second=row.candidateIdFields.total>1 ?
            unsigned(row.candidateIdFields.offsets[1]) : missing;
        const auto previous=row.previousIdFields.total ?
            unsigned(row.previousIdFields.offsets[0]) : missing;
        std::snprintf(line,sizeof(line),
            "LOOT_NATIVE_ROW_HANDOFF_SAMPLE version=1.0.0 phase=%s "
            "order=%u eventUnitId=%u priorEventUnitId=%u row=0x%llX "
            "readState=%u stride=0x2E8 alignedU32CandidateHits=%u "
            "candidateOffset0=0x%X candidateOffset1=0x%X "
            "priorIdHits=%u priorIdOffset0=0x%X "
            "interpretation=raw-field-candidates-not-item-ownership "
            "nativeWrites=0",
            phase,row.order,id,row.previousEventUnitId,
            static_cast<unsigned long long>(row.element),
            unsigned(row.handoffReadState),row.candidateIdFields.total,
            first,second,row.previousIdFields.total,previous);
        Emit(line);
    }
    std::snprintf(line,sizeof(line),
        "LOOT_NATIVE_ROW_HANDOFF_SUMMARY version=1.0.0 phase=%s "
        "samples=%zu rowReadable=%u rowUnreadable=%u noItemEvent=%u "
        "samplesWithCandidateIdField=%u observedIdTransitions=%u "
        "direct32bitScan=diagnostic-only "
        "noMatchDoesNotExclude-indirect-pointer-or-upstream-ownership=1 "
        "perItemNativeBackground=DISABLED nativeWrites=0",
        phase,count,readable,unreadable,noEventId,hits,transitions);
    Emit(line);
}

// Independent small static code fingerprints, never an executable-memory
// scan and never on the hooked renderer's hot path. This locates the known
// array append/renderer handoff instructions, NOT a UnitAny ownership edge.
void ReportNativeRowHandoffCode() noexcept {
    constexpr std::array<std::uint8_t,7> append{{
        0x4C,0x8D,0xB1,0x68,0x01,0x00,0x00}};
    constexpr std::array<std::uint8_t,7> render{{
        0x49,0x8D,0xBF,0x68,0x01,0x00,0x00}};
    constexpr std::array<std::uint8_t,17> caller{{
        0x48,0x69,0x4C,0x24,0x40,0xE8,0x02,0x00,0x00,
        0x48,0x03,0x0F,0xE8,0x14,0x9C,0x05,0x00}};
    const char* build=Context?D2RL::GetBuildName(Context):nullptr;
    bool verified=false;
    if (build && std::string_view(build)=="93847" && Base) {
        std::array<std::uint8_t,7> actualAppend{},actualRender{};
        std::array<std::uint8_t,17> actualCaller{};
        verified=ReadSafe(0x88017A,actualAppend.data(),actualAppend.size()) &&
            ReadSafe(0x880B4A,actualRender.data(),actualRender.size()) &&
            ReadSafe(0x880BBB,actualCaller.data(),actualCaller.size()) &&
            actualAppend==append && actualRender==render &&
            actualCaller==caller;
    }
    char line[440]{};
    std::snprintf(line,sizeof(line),
        "LOOT_NATIVE_ROW_HANDOFF_CODE version=1.0.0 verified=%u "
        "build=93847 append=D2R+0x880160 appendContainer=RCX+0x168 "
        "drawContainer=R15+0x168 draw=D2R+0x880BC7 "
        "rowFrom=index*[RSP+0x40]*0x2E8+base-[RDI] "
        "dynamicContainerEquality=unproven itemPointerLink=unproven "
        "onMismatch=no-inference noHooksAdded=1 nativeWrites=0",
        verified?1U:0U);
    Emit(line);
}

// Report only after sampling is OFF, outside the native renderer. We MUST
// label V1 vs row equality as temporal-only if the V3 scope expired before
// renderer invocation (SoE 0.18.194 scopes around the formatter call).
void ReportNativeRowChainSamples(const char* phase,
    const std::array<NativeRowChainSample,NativeRowMaxSamplesPerPhase>& chain,
    std::size_t count) noexcept {
    LARGE_INTEGER frequency{};
    const bool hasFrequency=QueryPerformanceFrequency(&frequency)!=0 &&
        frequency.QuadPart>0;
    char line[1050]{};
    std::uint32_t rendererScoped{},builderMatches{},eventTransitions{},geometryTransitions{};
    std::uint32_t previousId{};
    std::array<std::int32_t,4> previousRect{};
    for(std::size_t i=0;i<count;++i) {
        const auto& e=chain[i].event.latest;
        const auto& sample=chain[i];
        if(e.builderScopeMatches) ++builderMatches;
        if(sample.rendererScopeState==2) ++rendererScoped;
        if(i && e.unitId && previousId && e.unitId!=previousId) ++eventTransitions;
        if(i && sample.rect!=previousRect) ++geometryTransitions;
        previousId=e.unitId;
        previousRect=sample.rect;
        const bool qpcValid=hasFrequency && e.qpc>0 && sample.qpc>=e.qpc;
        const auto ageUs=qpcValid ? static_cast<std::uint64_t>(
            (static_cast<long double>(sample.qpc-e.qpc)*1000000.0L)/
            static_cast<long double>(frequency.QuadPart)) : 0U;
        const auto relation=NativeRowLabelCorrelation::Compare(
            std::string_view(reinterpret_cast<const char*>(sample.text.raw.data()),
                std::min<std::size_t>(sample.text.size,sample.text.raw.size())),
            std::string_view(reinterpret_cast<const char*>(e.source.data()),
                std::min<std::size_t>(e.sourceLength,e.source.size())));
        const bool sameThread=e.thread!=0 && e.thread==sample.thread;
        const char* ownership=(sample.rendererScopeState==2 &&
            sample.rendererScopeMatchesEvent && e.builderScopeMatches &&
            qpcValid && ageUs<=500000U && sameThread &&
            (relation==NativeRowLabelCorrelation::Relation::Exact ||
             relation==NativeRowLabelCorrelation::Relation::ColorPrefixOnly)) ?
            "scope-overlap-candidate-not-upstream-proven" :
            "temporal-only-no-row-item-ownership";
        std::snprintf(line,sizeof(line),
            "LOOT_NATIVE_ROW_CHAIN_SAMPLE version=1.0.0 phase=%s order=%u "
            "eventSeq=%llu eventUnitId=%u eventClassId=%u "
            "eventQpc=%lld renderQpc=%lld eventAgeUs=%llu ageValid=%u "
            "sameThread=%u builderScopeState=%u builderScopeId=%u builderScopeMatch=%u "
            "rendererScopeState=%u rendererScopeId=%u rendererScopeEventMatch=%u "
            "element=0x%llX rect=%ld,%ld,%ld,%ld alphaBits=%08X "
            "rowTextRelation=%s rowTextPreview='%s' ownership=%s nativeWrites=0",
            phase,sample.order,static_cast<unsigned long long>(e.sequence),
            e.unitId,e.classId,static_cast<long long>(e.qpc),
            static_cast<long long>(sample.qpc),
            static_cast<unsigned long long>(ageUs),qpcValid?1U:0U,
            sameThread?1U:0U,unsigned(e.builderScopeState),
            e.builderScopeUnitId,e.builderScopeMatches?1U:0U,
            unsigned(sample.rendererScopeState),sample.rendererScopeUnitId,
            sample.rendererScopeMatchesEvent?1U:0U,
            static_cast<unsigned long long>(sample.element),
            static_cast<long>(sample.rect[0]),static_cast<long>(sample.rect[1]),
            static_cast<long>(sample.rect[2]),static_cast<long>(sample.rect[3]),
            unsigned(sample.colorBits[3]),NativeRowLabelCorrelation::Name(relation),
            sample.text.preview.data(),ownership);
        Emit(line);
    }
    std::snprintf(line,sizeof(line),
        "LOOT_NATIVE_ROW_CHAIN_SUMMARY version=1.0.0 phase=%s "
        "samples=%zu builderScopeMatchesAtSample=%u rendererScopeActive=%u "
        "observedEventUnitTransitions=%u observedGeometryTransitions=%u "
        "next=trace-actual-builder-to-row-handoff-if-renderer-scope-empty "
        "perItemNativeBackground=DISABLED nativeWrites=0",
        phase,count,builderMatches,rendererScoped,eventTransitions,geometryTransitions);
    Emit(line);
}

void ReportNativeRowRuntime() noexcept {
    NativeRowBgTrialEnabled.store(false,std::memory_order_release);
    NativeRowActivePhase.store(NativeRowPhase::Off,std::memory_order_release);
    NativeRowDeadline.store(0,std::memory_order_release);
    NativeRowNextSampleAt.store(0,std::memory_order_release);
    char line[650]{};
    std::snprintf(line,sizeof(line),
        "LOOT_NATIVE_ROW_REPORT_BEGIN version=1.0.0 hookInstalled=%u sampledCalls=%llu otherCallerCalls=%llu contention=%llu perPhaseMaxSamples=8 samplingIntervalMs=180 autoHoverStaticDumps=0 itemAssociation=unverified observerColorWrites=0 trialColorWrites=%llu overlays=0 globalRectHook=0",
        NativeRowRendererHookInstalled.load()?1U:0U,
        static_cast<unsigned long long>(NativeRowCalls.load()),
        static_cast<unsigned long long>(NativeRowCallerMismatch.load()),
        static_cast<unsigned long long>(NativeRowLockContention.load()),
        static_cast<unsigned long long>(NativeRowBgWrites.load()));
    Emit(line);
    std::array<std::array<std::uintptr_t,NativeRowMaxRecords>,NativeRowPhaseCount>
        phaseAddresses{};
    std::array<std::size_t,NativeRowPhaseCount> phaseSizes{};
    for (std::size_t phase=0;phase<NativeRowPhaseCount;++phase) {
        std::array<NativeRowRecord,NativeRowMaxRecords> saved{};
        std::array<NativeRowChainSample,NativeRowMaxSamplesPerPhase> chain{};
        std::size_t chainSize{};
        std::size_t size{};
        std::uint64_t calls{},failures{},overflow{},invalid{},other{};
        {
            std::lock_guard lock(NativeRowBuckets[phase].mutex);
            const auto& b=NativeRowBuckets[phase];
            saved=b.records;size=b.size;
            chain=b.chain;chainSize=b.chainSize;
            calls=b.calls;failures=b.readFailures;
            overflow=b.overflow;invalid=b.invalidColor;
            other=b.otherCallers;
        }
        for(std::size_t i=0;i<size;++i)
            phaseAddresses[phase][i]=saved[i].address;
        phaseSizes[phase]=size;
        std::sort(saved.begin(),saved.begin()+size,
            [](const NativeRowRecord& a,const NativeRowRecord& b) noexcept {
                return a.calls>b.calls;
            });
        std::snprintf(line,sizeof(line),
            "LOOT_NATIVE_ROW_PHASE_SUMMARY version=1.0.0 phase=%s calls=%llu uniqueObjects=%zu readFailures=%llu overflow=%llu invalidFloat4=%llu unrelatedCallers=%llu caller=0x880BCC rawRectOffsets=+0x50,+0x54,+0x58,+0x5C rgbaOffset=+0x168 objectIdentity=unproven",
            NativeRowPhaseNames[phase],
            static_cast<unsigned long long>(calls),size,
            static_cast<unsigned long long>(failures),
            static_cast<unsigned long long>(overflow),
            static_cast<unsigned long long>(invalid),
            static_cast<unsigned long long>(other));
        Emit(line);
        ReportNativeRowChainSamples(NativeRowPhaseNames[phase],chain,chainSize);
        ReportNativeRowHandoffSamples(NativeRowPhaseNames[phase],chain,chainSize);
        ReportNativeRowAppendMatch(phase,chain,chainSize);
        ReportNativeRowLatestAppend(phase,chain,chainSize);
        // Copy phase evidence under its own mutex; never hold it across Emit.
        std::array<NativeRowLabelEvent,NativeRowMaxDistinctUnits> units{};
        std::size_t unitCount{};
        std::uint64_t eventCalls{},eventOverflow{},emptySources{};
        std::uint64_t builderScopedMatches{},builderScopedMisses{},builderNoScope{};
        {
            std::lock_guard lock(NativeRowLabelPhases[phase].mutex);
            const auto& labels=NativeRowLabelPhases[phase];
            units=labels.units;
            unitCount=labels.uniqueCount;
            eventCalls=labels.callbacks;
            eventOverflow=labels.overflow;
            emptySources=labels.withoutText;
            builderScopedMatches=labels.builderScopedMatches;
            builderScopedMisses=labels.builderScopedMisses;
            builderNoScope=labels.builderNoScope;
        }
        std::snprintf(line,sizeof(line),
            "LOOT_NATIVE_ROW_BUILDER_SCOPE version=1.0.0 phase=%s "
            "v3Match=%llu v3Mismatch=%llu v3Empty=%llu callbacks=%llu "
            "meaning=scope-inside-SoE-original-formatter-only not-renderer-ownership nativeWrites=0",
            NativeRowPhaseNames[phase],
            static_cast<unsigned long long>(builderScopedMatches),
            static_cast<unsigned long long>(builderScopedMisses),
            static_cast<unsigned long long>(builderNoScope),
            static_cast<unsigned long long>(eventCalls));
        Emit(line);
        // Pairs of different unit IDs with identical complete borrowed
        // source bytes are explicitly ambiguous even when names match.
        std::uint32_t duplicatePairs{};
        for(std::size_t a=0;a<unitCount;++a)
            for(std::size_t b=a+1;b<unitCount;++b)
                if(units[a].unitId!=units[b].unitId &&
                    units[a].sourceLength &&
                    units[a].sourceLength<=units[a].source.size() &&
                    units[a].sourceLength==units[b].sourceLength &&
                    std::memcmp(units[a].source.data(),units[b].source.data(),
                        units[a].sourceLength)==0)
                    ++duplicatePairs;
        std::snprintf(line,sizeof(line),
            "LOOT_NATIVE_ROW_LABEL_EVENTS version=1.0.0 phase=%s "
            "callbacks=%llu distinctUnitIds=%zu duplicateSourcePairs=%u "
            "unitOverflow=%llu emptySource=%llu eventMutexMissesGlobal=%llu "
            "sampleMutexMissesGlobal=%llu source=SoE-V1-or-standalone "
            "itemOwnership=unproven nativeWrites=0",
            NativeRowPhaseNames[phase],
            static_cast<unsigned long long>(eventCalls),unitCount,
            duplicatePairs,static_cast<unsigned long long>(eventOverflow),
            static_cast<unsigned long long>(emptySources),
            static_cast<unsigned long long>(NativeRowLabelEventContention.load()),
            static_cast<unsigned long long>(NativeRowLabelSnapshotContention.load()));
        Emit(line);
        for(std::size_t i=0;i<std::min(size,NativeRowPrintRecords);++i) {
            const auto& v=saved[i];
            std::array<float,4> rgba{};
            std::memcpy(rgba.data(),v.firstColorBits.data(),sizeof(rgba));
            std::snprintf(line,sizeof(line),
                "LOOT_NATIVE_ROW_SAMPLE phase=%s ordinal=%zu element=0x%llX tid=%lu hits=%llu rectFirst=%ld,%ld,%ld,%ld rectLast=%ld,%ld,%ld,%ld rgbaFirst=%.3f,%.3f,%.3f,%.3f firstColorRaw=%08X,%08X,%08X,%08X colorTransitions=%u geometryTransitions=%u provenance=temporal-candidate-not-item-identity",
                NativeRowPhaseNames[phase],i,
                static_cast<unsigned long long>(v.address),
                static_cast<unsigned long>(v.thread),
                static_cast<unsigned long long>(v.calls),
                static_cast<long>(v.firstRect[0]),static_cast<long>(v.firstRect[1]),
                static_cast<long>(v.firstRect[2]),static_cast<long>(v.firstRect[3]),
                static_cast<long>(v.lastRect[0]),static_cast<long>(v.lastRect[1]),
                static_cast<long>(v.lastRect[2]),static_cast<long>(v.lastRect[3]),
                double(rgba[0]),double(rgba[1]),double(rgba[2]),double(rgba[3]),
                unsigned(v.firstColorBits[0]),unsigned(v.firstColorBits[1]),
                unsigned(v.firstColorBits[2]),unsigned(v.firstColorBits[3]),
                unsigned(v.colorTransitions),unsigned(v.geometryTransitions));
            Emit(line);
            // Header words are logged even when the guessed MSVC string
            // layout is implausible, allowing a later offline correction.
            std::array<std::uint64_t,4> raw0{},raw1{};
            std::memcpy(raw0.data(),v.firstTextHeaders.data(),sizeof(raw0));
            std::memcpy(raw1.data(),v.firstTextHeaders.data()+0x28,sizeof(raw1));
            char witnessLine[1600]{};
            std::snprintf(witnessLine,sizeof(witnessLine),
                "LOOT_NATIVE_ROW_TEXT_WITNESS version=1.0.0 phase=%s ordinal=%zu element=0x%llX headerTransitions=%u "
                "text0State=%u text0Size=%llu text0Capacity=%llu text0EncodedCap=%016llX text0Preview='%s' text0Hex=%s "
                "text1State=%u text1Size=%llu text1Capacity=%llu text1EncodedCap=%016llX text1Preview='%s' text1Hex=%s "
                "lastText0State=%u lastText0Size=%llu lastText0Preview='%s' lastText0Hex=%s "
                "lastText1State=%u lastText1Size=%llu lastText1Preview='%s' lastText1Hex=%s "
                "header0=%016llX,%016llX,%016llX,%016llX "
                "header1=%016llX,%016llX,%016llX,%016llX "
                "interpretation=candidate-tagged-inline-or-heap-strings-not-item-identity no-native-writes=1",
                NativeRowPhaseNames[phase],i,
                static_cast<unsigned long long>(v.address),v.textHeaderTransitions,
                unsigned(v.firstText[0].state),
                static_cast<unsigned long long>(v.firstText[0].size),
                static_cast<unsigned long long>(v.firstText[0].capacity),
                static_cast<unsigned long long>(v.firstText[0].encodedCapacity),
                v.firstText[0].preview.data(),v.firstText[0].rawHex.data(),
                unsigned(v.firstText[1].state),
                static_cast<unsigned long long>(v.firstText[1].size),
                static_cast<unsigned long long>(v.firstText[1].capacity),
                static_cast<unsigned long long>(v.firstText[1].encodedCapacity),
                v.firstText[1].preview.data(),v.firstText[1].rawHex.data(),
                unsigned(v.lastText[0].state),
                static_cast<unsigned long long>(v.lastText[0].size),
                v.lastText[0].preview.data(),v.lastText[0].rawHex.data(),
                unsigned(v.lastText[1].state),
                static_cast<unsigned long long>(v.lastText[1].size),
                v.lastText[1].preview.data(),v.lastText[1].rawHex.data(),
                static_cast<unsigned long long>(raw0[0]),
                static_cast<unsigned long long>(raw0[1]),
                static_cast<unsigned long long>(raw0[2]),
                static_cast<unsigned long long>(raw0[3]),
                static_cast<unsigned long long>(raw1[0]),
                static_cast<unsigned long long>(raw1[1]),
                static_cast<unsigned long long>(raw1[2]),
                static_cast<unsigned long long>(raw1[3]));
            Emit(witnessLine);
            auto duplicatesFor=[&](const NativeRowLabelSnapshot& label) noexcept {
                const auto& source=label.latest;
                if(!source.unitId || !source.sourceLength ||
                    source.sourceLength>source.source.size()) return 0U;
                std::uint32_t duplicates{};
                for(std::size_t j=0;j<unitCount;++j) {
                    const auto& other=units[j];
                    if(other.unitId==source.unitId ||
                        other.sourceLength!=source.sourceLength) continue;
                    if(std::memcmp(other.source.data(),source.source.data(),
                        source.sourceLength)==0) ++duplicates;
                }
                return duplicates;
            };
            ReportNativeRowItemEvent(NativeRowPhaseNames[phase],i,
                v.address,"first",v.firstText[0],v.firstLabel,
                v.firstTick,v.thread,duplicatesFor(v.firstLabel),
                eventOverflow!=0);
            ReportNativeRowItemEvent(NativeRowPhaseNames[phase],i,
                v.address,"last",v.lastText[0],v.lastLabel,
                v.lastTick,v.thread,duplicatesFor(v.lastLabel),
                eventOverflow!=0);
        }
    }
    std::size_t hiddenOnly{},hiddenAndAway{},hiddenAndInventory{};
    for(std::size_t i=0;i<phaseSizes[0];++i) {
        const auto ptr=phaseAddresses[0][i];
        const auto contains=[&](std::size_t group) noexcept {
            for(std::size_t j=0;j<phaseSizes[group];++j)
                if(phaseAddresses[group][j]==ptr)return true;
            return false;
        };
        const bool away=contains(1),inventory=contains(2);
        if (!away && !inventory)++hiddenOnly;
        if (away)++hiddenAndAway;
        if (inventory)++hiddenAndInventory;
    }
    std::snprintf(line,sizeof(line),
        "LOOT_NATIVE_ROW_CORRELATION version=1.0.0 hiddenOnlyObjects=%zu hiddenAndAwayObjects=%zu hiddenAndInventoryObjects=%zu comparison=pointer-equality-only phaseCoverage=manual-regions-not-item-provenance no-color-writes=1",
        hiddenOnly,hiddenAndAway,hiddenAndInventory);
    Emit(line);
    ReportNativeRowHandoffCode();
    Emit("LOOT_NATIVE_ROW_REPORT_END version=1.0.0 next=audit-latest-append-invocation-association item-ownership=NOT-PROVEN no-native-writes=1 native-global-rectangle-hook=0");
    FlushCapture();
}

// The original hooked helper at D2R+0x36EF50 has a verified 1-argument
// signature, and its *trampoline* is used here (never the patched entry).
// The formatter's native item pointer is only passed after positive, freshly
// captured type/record-ID checks. That pointer equivalence is what the probe
// tests; an item-code result is evidence, not a hardcoded assumption.
// No calls are made unless `code-arm` AND an explicit 8-second capture are on.
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

void ProbeFormatterItemCode(void* unit, bool qualified,
                            FormatterObservation& temp) noexcept {
    if (!CodeBridgeArmed.load(std::memory_order_acquire)) return;
    temp.codeAttempted = true;
    if (!qualified || !OriginalGetItemCode || !HookInstalled.load(std::memory_order_acquire) ||
        !temp.unitOk || !temp.postOk || temp.nativeFirst6[0] != 4) {
        CodeBridgeGuardReject.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint32_t recordId{};
    std::memcpy(&recordId, temp.post.data() + 0x10, sizeof(recordId));
    if (recordId != temp.nativeFirst6[2]) {
        CodeBridgeGuardReject.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    temp.codeGuardPassed = true;
    CodeBridgeAttempts.fetch_add(1, std::memory_order_relaxed);
    // Do not call HookGetItemCode: the trampoline avoids recursion and lets
    // the existing hook preserve its separate normal-call telemetry.
    temp.codeValue = OriginalGetItemCode(unit);
    temp.codeValid = PrintableItemCode(temp.codeValue);
    if (!temp.codeValid) {
        CodeBridgeInvalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    CodeBridgeSuccess.fetch_add(1, std::memory_order_relaxed);
    if (temp.codeValue == DivineCode) {
        CodeBridgeDivine.fetch_add(1, std::memory_order_relaxed);
        if (temp.nativeFirst6[1] != DivineNativeClassId)
            CodeBridgeDivineMismatch.fetch_add(1, std::memory_order_relaxed);
    } else if (temp.nativeFirst6[1] == ObservedMapNativeClassId) {
        // Count a map *class candidate*; the emitted code is the experiment.
        CodeBridgeMap.fetch_add(1, std::memory_order_relaxed);
    } else {
        CodeBridgeOther.fetch_add(1, std::memory_order_relaxed);
    }
}

// A guarded proof-of-concept, not a general-purpose filter. Only invoked by
// the formatter hook AFTER the game has populated the UI record. No item data
// or tooltip storage is written. This changes at most 11 bytes (including NUL)
// of the explicitly identified ground-label record, with equal-size text.
void TryRenameDivineLabel(void* unit, void* record, bool sourceCall,
                          bool paired, std::uint8_t originalResult) noexcept {
    if (!RenameArmed.load(std::memory_order_acquire) ||
        !sourceCall || !paired || originalResult == 0 || !unit || !record)
        return;
    RenameQualified.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t nativeHeader[4]{};
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(), unit, nativeHeader,
            sizeof(nativeHeader), &copied) || copied != sizeof(nativeHeader)) {
        RenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Native object type 4 (item), the class ID observed alongside Divine Orb.
    if (nativeHeader[0] != 4 || nativeHeader[1] != DivineNativeClassId) {
        RenameNonDivine.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto recordAddress = reinterpret_cast<std::uintptr_t>(record);
    if (recordAddress > UINTPTR_MAX - CandidateTextOffset - sizeof(OriginalDivineLabel)) {
        RenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint32_t recordId{};
    copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(recordAddress + 0x10),
            &recordId, sizeof(recordId), &copied) || copied != sizeof(recordId)) {
        RenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (recordId != nativeHeader[2]) {
        RenameIdMismatch.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto textAddress = recordAddress + CandidateTextOffset;
    char currentText[sizeof(OriginalDivineLabel)]{};
    copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(textAddress), currentText,
            sizeof(currentText), &copied) || copied != sizeof(currentText)) {
        RenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (std::memcmp(currentText, OriginalDivineLabel, sizeof(currentText)) != 0) {
        RenameTextMismatch.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The original formatter just wrote this region, but still verify that
    // it remains fully committed/writable. No VirtualProtect, code writes,
    // or brute-force fallback if the memory has changed protection.
    MEMORY_BASIC_INFORMATION memory{};
    auto* textPointer = reinterpret_cast<void*>(textAddress);
    if (!VirtualQuery(textPointer, &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        RenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto protection = memory.Protect & 0xFFU;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE &&
        protection != PAGE_EXECUTE_WRITECOPY) {
        RenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) +
        memory.RegionSize;
    if (regionEnd < textAddress || regionEnd - textAddress < sizeof(ReplacementDivineLabel)) {
        RenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Same-length, NUL-terminated replacement; all other record fields stay
    // native. Geometry was measured from the original string: do not treat
    // this as a production implementation for variable-length replacements.
    std::memcpy(textPointer, ReplacementDivineLabel, sizeof(ReplacementDivineLabel));
    RenameWrites.fetch_add(1, std::memory_order_relaxed);
}

// Opt-in 0.1.52 code-matched ground-name PoC. The existing class-ID-based
// rename remains separate for regression comparison. A rule identifies the
// underlying item by the original D2R item-code trampoline, not name/color or
// class ID. The UI record is a transient formatter output, not item storage.
void TryCodeRenameGroundLabel(void* unit, void* record, bool sourceCall,
                              bool paired, std::uint8_t originalResult) noexcept {
    if (!CodeRenameArmed.load(std::memory_order_acquire) ||
        !sourceCall || !paired || originalResult == 0 || !unit || !record)
        return;
    CodeRenameQualified.fetch_add(1, std::memory_order_relaxed);
    if (!OriginalGetItemCode || !HookInstalled.load(std::memory_order_acquire)) {
        CodeRenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint32_t unitHeader[4]{};
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(), unit, unitHeader,
            sizeof(unitHeader), &copied) || copied != sizeof(unitHeader) ||
        unitHeader[0] != 4) {
        CodeRenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto recordAddress = reinterpret_cast<std::uintptr_t>(record);
    if (recordAddress > UINTPTR_MAX - CandidateTextOffset -
            sizeof(ReplacementDivineLabel)) {
        CodeRenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint32_t recordId{};
    copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(recordAddress + 0x10),
            &recordId, sizeof(recordId), &copied) || copied != sizeof(recordId)) {
        CodeRenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (recordId != unitHeader[2]) {
        CodeRenameIdMismatch.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // The item-code bridge successfully tested the same pointer in 0.1.19:
    // 'divo' and 'mp04' returned against their actual displayed ground names.
    // Use the trampoline directly; never recurse through HookGetItemCode.
    CodeRenameLookups.fetch_add(1, std::memory_order_relaxed);
    const auto itemCode = CanonicalItemCode(OriginalGetItemCode(unit));
    if (!PrintableItemCode(itemCode)) {
        CodeRenameInvalid.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const CodeNameRule* rule{};
    for (const auto& candidate : CodeNameRules) {
        if (candidate.code == itemCode) {
            rule = &candidate;
            break;
        }
    }
    if (!rule) {
        CodeRenameNoRule.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    CodeRenameRuleMatches.fetch_add(1, std::memory_order_relaxed);
    // Geometry still reflects the game's original text measurement. Only
    // same-BYTE-LENGTH substitutions are allowed in this first PoC. No exact
    // 'Divine Orb' string check: the selection is solely native item code.
    if (rule->replacementBytes < 2 ||
        rule->replacementBytes > CandidateTextMaximum ||
        rule->replacement[rule->replacementBytes - 1] != '\0') {
        CodeRenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto textAddress = recordAddress + CandidateTextOffset;
    std::array<char, CandidateTextMaximum> text{};
    copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(textAddress), text.data(),
            text.size(), &copied) || copied != text.size()) {
        CodeRenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto* terminator = static_cast<const char*>(
        std::memchr(text.data(), '\0', text.size()));
    if (!terminator || static_cast<std::size_t>(terminator - text.data()) + 1 !=
            rule->replacementBytes) {
        CodeRenameTextLengthMismatch.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Do not alter metadata, UI geometry, text length, item memory, quality,
    // inventory labels, or the formatter input. Verify writable record bounds.
    MEMORY_BASIC_INFORMATION memory{};
    auto* destination = reinterpret_cast<void*>(textAddress);
    if (!VirtualQuery(destination, &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        CodeRenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto protection = memory.Protect & 0xFFU;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE &&
        protection != PAGE_EXECUTE_WRITECOPY) {
        CodeRenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto regionStart = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    if (regionStart > textAddress || memory.RegionSize > UINTPTR_MAX - regionStart ||
        regionStart + memory.RegionSize < textAddress ||
        regionStart + memory.RegionSize - textAddress < rule->replacementBytes) {
        CodeRenameReadFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::memcpy(destination, rule->replacement, rule->replacementBytes);
    CodeRenameWrites.fetch_add(1, std::memory_order_relaxed);
}

// The canonical runtime configuration is filter.json beside the DLL.
// Previous production/probe filenames remain read-only migration fallbacks when
// the canonical file is absent.
bool ResolveFilterConfigPath() noexcept {
    HMODULE self{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&TryCodeRenameGroundLabel), &self) || !self)
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

// 0.1.52: READ-ONLY hover-route comparison. D2R's hover-only text can use
// different call sites (or different functions entirely). Observe ONLY the
// already fingerprinted formatter, inner name writer and shared painter.
// No new native hooks, guessed calling conventions, item writes or hook I/O.
enum class HoverStage : std::size_t { Formatter=0, NameWriter=1, Painter=2 };
constexpr std::size_t HoverStageCount=3;
constexpr std::size_t HoverCallerSlots=32;
constexpr ULONGLONG HoverProbeDurationMs=12'000;
struct HoverCallerRecord {
    std::uintptr_t returnRva{};
    std::uint64_t hits{};
    std::uint64_t paired{};
    std::uint32_t firstArg{}; // formatter flags / name-writer buffer size
    std::uint32_t header0{}; // first dword at unit, read-only, not a validated code
    std::uint32_t header2{}; // potential unit id, not assumed for unknown callers
    std::uint32_t recordId{};
    std::uint32_t samples{};
    std::uint64_t verifiedDivine{};
    std::uint64_t verifiedMap{};
    std::uint64_t verifiedOther{};
    std::uint64_t identityUnresolved{};
    std::uint32_t firstVerifiedCode{};
    char firstName[84]{};
    char targetName[84]{};
};
std::mutex HoverProbeMutex;
std::atomic_bool HoverProbeArmed{false};
std::atomic<ULONGLONG> HoverProbeDeadline{};
std::array<std::array<HoverCallerRecord,HoverCallerSlots>,HoverStageCount> HoverProbeCallers{};
std::array<std::size_t,HoverStageCount> HoverProbeSizes{};
std::array<std::uint64_t,HoverStageCount> HoverProbeHits{};
std::array<std::uint64_t,HoverStageCount> HoverProbeOverflow{};
std::array<char,24> HoverProbePhase{};

bool HoverTextIsTarget(std::string_view text) noexcept {
    for (std::size_t i=0;i<text.size();++i) {
        constexpr std::string_view needles[]{"divine", "custom map", "high value"};
        for (const auto needle:needles) {
            if (needle.size()>text.size()-i) continue;
            bool equal=true;
            for (std::size_t j=0;j<needle.size();++j) {
                auto c=text[i+j];
                if (c>='A' && c<='Z') c=static_cast<char>(c+('a'-'A'));
                if (c!=needle[j]) {equal=false;break;}
            }
            if(equal) return true;
        }
    }
    return false;
}

// Already implemented later: the painter only exposes a rendered label record,
// so the code must be verified against the recent formatter unitId AND exact
// prepared name. This bridge is not a mouse-targeting API.
bool GetGroundIdentity(std::uint32_t id, std::string_view visibleName,
                       std::uint32_t& code,
                       std::uint32_t* quantityOut=nullptr,
                       std::uint32_t* classIdOut=nullptr,
                       RuleEngine::Item* scalarOut=nullptr) noexcept;

// Untrusted buffers are copied, never modified. The record offsets are only
// inspected when the arguments fit the previously verified pair structure.
void ObserveHoverRoute(HoverStage stage,std::uintptr_t caller,void* unit,
    void* output,void* record,std::uint32_t argument,void* color=nullptr) noexcept {
    if (!HoverProbeArmed.load(std::memory_order_relaxed) ||
        GetTickCount64()>=HoverProbeDeadline.load(std::memory_order_relaxed)) return;
    if (!HoverProbeMutex.try_lock()) return;
    std::lock_guard<std::mutex> hold(HoverProbeMutex,std::adopt_lock);
    if (!HoverProbeArmed.load(std::memory_order_relaxed) ||
        GetTickCount64()>=HoverProbeDeadline.load(std::memory_order_relaxed)) return;
    const auto group=static_cast<std::size_t>(stage);
    ++HoverProbeHits[group];
    const auto rva=caller>=Base && caller-Base<ImageSize?caller-Base:0;
    HoverCallerRecord* row{};
    for (std::size_t i=0;i<HoverProbeSizes[group];++i) {
        if (HoverProbeCallers[group][i].returnRva==rva) {
            row=&HoverProbeCallers[group][i];break;
        }
    }
    if(!row) {
        if(HoverProbeSizes[group]>=HoverCallerSlots) {++HoverProbeOverflow[group];return;}
        row=&HoverProbeCallers[group][HoverProbeSizes[group]++];
        *row={};
        row->returnRva=rva;
        row->firstArg=argument;
    }
    ++row->hits;
    const auto out=reinterpret_cast<std::uintptr_t>(output);
    const auto rec=reinterpret_cast<std::uintptr_t>(record);
    bool paired=false;
    std::uintptr_t textAddress{};
    std::size_t textLength=80;
    if(stage==HoverStage::Formatter) {
        paired=rec && out && rec<=UINTPTR_MAX-0x28-80 && out==rec+0x24;
        if(paired) textAddress=rec+0x28;
    } else if(stage==HoverStage::NameWriter) {
        paired=out && argument>=8 && argument<=0x1000 && out<=UINTPTR_MAX-84;
        if(paired) {
            textAddress=out+4;
            textLength=std::min<std::size_t>(80,argument-4);
        }
    } else {
        paired=rec && out && color && rec<=UINTPTR_MAX-0x28-80 &&
            out==rec+0x24 &&
            reinterpret_cast<std::uintptr_t>(color)==rec+GroundColorOffset;
        if(paired) textAddress=rec+0x28;
    }
    if(paired) ++row->paired;
    // Phase-local read-only identity differential. The native item is valid
    // ONLY on the verified formatter/name-writer path; painter identity uses
    // the already qualified unitId + exact label-name bridge. No address-only
    // inference and no item, tooltip or label writes.
    std::uint32_t verifiedCode{};
    bool identityFound=false;
    if(paired && (stage==HoverStage::Formatter || stage==HoverStage::NameWriter) &&
       unit && OriginalGetItemCode && HookInstalled.load(std::memory_order_acquire)) {
        const bool knownWriter=stage==HoverStage::NameWriter &&
            caller==Base+InnerNameWriterCallerRva+DirectCallBytes &&
            argument==InnerNameBufferBytes;
        const bool knownFormatter=stage==HoverStage::Formatter &&
            (caller==Base+LabelFormatterCallRva+DirectCallBytes ||
             caller==Base+LabelFormatterSecondCallRva+DirectCallBytes);
        if(knownWriter || knownFormatter) {
            std::uint32_t header[4]{};
            SIZE_T copied{};
            const bool validUnit=ReadProcessMemory(GetCurrentProcess(),unit,header,
                sizeof(header),&copied) && copied==sizeof(header) && header[0]==4;
            bool validPair=validUnit;
            const auto recordForId=stage==HoverStage::Formatter?rec:out-0x24;
            if(validPair && recordForId<=UINTPTR_MAX-0x14) {
                std::uint32_t recordId{};
                copied=0;
                validPair=ReadProcessMemory(GetCurrentProcess(),
                    reinterpret_cast<const void*>(recordForId+0x10),
                    &recordId,sizeof(recordId),&copied) &&
                    copied==sizeof(recordId) && recordId==header[2] && recordId!=0;
            } else validPair=false;
            if(validPair) {
                verifiedCode=CanonicalItemCode(OriginalGetItemCode(unit));
                identityFound=PrintableItemCode(verifiedCode);
            }
        }
    } else if(paired && stage==HoverStage::Painter &&
              (caller==Base+0x1517AF6 || caller==Base+0x1519E46) &&
              rec<=UINTPTR_MAX-0x28-80) {
        std::uint32_t id{};
        std::array<char,80> name{};
        SIZE_T copied{};
        const bool idOk=ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(rec+0x10),&id,sizeof(id),&copied) &&
            copied==sizeof(id) && id!=0;
        copied=0;
        const bool nameOk=ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(rec+0x28),name.data(),name.size(),&copied) &&
            copied==name.size() && std::memchr(name.data(),'\0',name.size())!=nullptr;
        if(idOk && nameOk) {
            const auto length=strnlen_s(name.data(),name.size());
            identityFound=GetGroundIdentity(id,std::string_view(name.data(),length),
                verifiedCode);
        }
    }
    if(paired) {
        if(!identityFound) ++row->identityUnresolved;
        else {
            if(row->firstVerifiedCode==0) row->firstVerifiedCode=verifiedCode;
            if(verifiedCode==DivineCode) ++row->verifiedDivine;
            else if(verifiedCode==PackFilterCode("mp04")) ++row->verifiedMap;
            else ++row->verifiedOther;
        }
    }
    if(row->samples>=32 || (!row->firstName[0] && !paired)) return;
    // Avoid reading this uncertain native unit on every render frame.
    if(row->samples==0 && unit) {
        std::uint32_t firstFour[4]{};
        SIZE_T copied{};
        if(ReadProcessMemory(GetCurrentProcess(),unit,firstFour,sizeof(firstFour),&copied) &&
            copied==sizeof(firstFour)) {
            row->header0=firstFour[0];
            row->header2=firstFour[2];
        }
    }
    if(row->samples==0 && (stage==HoverStage::Formatter || stage==HoverStage::Painter) &&
        paired && rec<=UINTPTR_MAX-0x14) {
        SIZE_T copied{};
        (void)ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(rec+0x10),&row->recordId,
            sizeof(row->recordId),&copied);
    }
    if(!textAddress || (row->firstName[0] && row->targetName[0])) return;
    ++row->samples;
    std::array<char,80> bytes{};
    SIZE_T copied{};
    if(!ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(textAddress),bytes.data(),textLength,&copied) ||
        copied!=textLength) return;
    const auto* terminator=static_cast<const char*>(
        std::memchr(bytes.data(),'\0',textLength));
    if(!terminator || terminator==bytes.data()) return;
    const auto length=static_cast<std::size_t>(terminator-bytes.data());
    const auto target=HoverTextIsTarget(std::string_view(bytes.data(),length));
    char printable[84]{};
    std::size_t used{};
    for(std::size_t i=0;i<length && used<sizeof(printable)-1;++i) {
        const unsigned char ch=static_cast<unsigned char>(bytes[i]);
        if(ch=='\n' || ch=='\r') printable[used++]='|';
        else if(ch>=0x20 && ch<=0x7e) printable[used++]=static_cast<char>(ch);
        else printable[used++]='?';
    }
    printable[used]=0;
    if(!row->firstName[0]) std::memcpy(row->firstName,printable,used+1);
    if(target && !row->targetName[0])
        std::memcpy(row->targetName,printable,used+1);
}

void ReportHoverProbe(bool stop) noexcept {
    if(stop) HoverProbeArmed.store(false,std::memory_order_release);
    std::lock_guard<std::mutex> lock(HoverProbeMutex);
    char message[512]{};
    std::snprintf(message,sizeof(message),
        "LOOT_HOVER_PROBE_BEGIN version=1.0.0 phase='%s' active=%u durationMs=%llu stages=formatter,name-writer,painter labelWrites=unchanged itemWrites=0 nativeHookAdds=0",
        HoverProbePhase.data(),HoverProbeArmed.load()?1U:0U,
        static_cast<unsigned long long>(HoverProbeDurationMs));
    Emit(message);
    constexpr const char* names[]{"formatter","name-writer","painter"};
    for(std::size_t group=0;group<HoverStageCount;++group) {
        std::snprintf(message,sizeof(message),
            "LOOT_HOVER_STAGE phase='%s' stage=%s calls=%llu distinctCallers=%zu overflow=%llu",
            HoverProbePhase.data(),names[group],
            static_cast<unsigned long long>(HoverProbeHits[group]),
            HoverProbeSizes[group],
            static_cast<unsigned long long>(HoverProbeOverflow[group]));
        Emit(message);
        for(std::size_t i=0;i<HoverProbeSizes[group];++i) {
            const auto& r=HoverProbeCallers[group][i];
            std::snprintf(message,sizeof(message),
                "LOOT_HOVER_SITE phase='%s' stage=%s returnRva=0x%llX calls=%llu paired=%llu divo=%llu mp04=%llu verifiedOther=%llu unresolved=%llu firstCode=0x%08X firstArg=%u unitHeader0=0x%X unitIdCandidate=%u recordIdCandidate=%u samples=%u sample='%s' target='%s'",
                HoverProbePhase.data(),names[group],
                static_cast<unsigned long long>(r.returnRva),
                static_cast<unsigned long long>(r.hits),
                static_cast<unsigned long long>(r.paired),
                static_cast<unsigned long long>(r.verifiedDivine),
                static_cast<unsigned long long>(r.verifiedMap),
                static_cast<unsigned long long>(r.verifiedOther),
                static_cast<unsigned long long>(r.identityUnresolved),
                r.firstVerifiedCode,r.firstArg,
                r.header0,r.header2,r.recordId,r.samples,r.firstName,r.targetName);
            Emit(message);
        }
    }
    Emit("LOOT_HOVER_PROBE_END note=compare-visible-hidden-inventory captures; zero-delta means this hook route was not used, not that item never rendered");
    FlushCapture();
}

void StartHoverProbe(std::string_view phase) noexcept {
    if(phase!="idle" && phase!="visible" && phase!="hidden" && phase!="inventory") {
        Emit("LOOT_HOVER_REFUSED usage: hover-probe-start idle|visible|hidden|inventory");return;
    }
    if(!FormatterHookInstalled.load(std::memory_order_acquire) ||
       !InnerNameHookInstalled.load(std::memory_order_acquire) ||
       !BackgroundPaintHookInstalled.load(std::memory_order_acquire)) {
        Emit("LOOT_HOVER_REFUSED install formatter-arm, geometry-hook, background-paint-observe first");return;
    }
    HoverProbeArmed.store(false,std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(HoverProbeMutex);
        for(auto& group:HoverProbeCallers) group.fill({});
        HoverProbeSizes.fill(0);HoverProbeHits.fill(0);HoverProbeOverflow.fill(0);
        HoverProbePhase.fill(0);
        std::memcpy(HoverProbePhase.data(),phase.data(),phase.size());
        HoverProbeDeadline.store(GetTickCount64()+HoverProbeDurationMs,
            std::memory_order_release);
        HoverProbeArmed.store(true,std::memory_order_release);
    }
    char message[260]{};
    std::snprintf(message,sizeof(message),
        "LOOT_HOVER_PROBE_ARMED version=1.0.0 phase='%s' durationMs=%llu formatter=0x1FA9F0 writer=0xCBEB0 painter=0x1FA8E0 readOnly=1 sourceUnchanged=1",
        HoverProbePhase.data(),static_cast<unsigned long long>(HoverProbeDurationMs));
    Emit(message);
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
    ObserveHoverRoute(HoverStage::NameWriter,returnAddress,unit,output,nullptr,bufferSize);
    GeometryTotalCalls.fetch_add(1, std::memory_order_relaxed);
    const auto mode = ActiveGeometryMode.load(std::memory_order_acquire);
    if (mode == GeometryMode::Off ||
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
    if (mode == GeometryMode::Rules) {
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
    } else if (code != DivineCode) {
        GeometryNoRule.fetch_add(1, std::memory_order_relaxed);
        return result;
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
    if (mode == GeometryMode::Observe) return result; // NEVER write
    const char* replacement = mode == GeometryMode::Rules ? configuredName :
        (mode == GeometryMode::Long ? LongDivineLabel : ShortDivineLabel);
    std::size_t replacementBytes = mode == GeometryMode::Rules ? configuredBytes :
        (mode == GeometryMode::Long ? sizeof(LongDivineLabel) :
            sizeof(ShortDivineLabel));
    // This native writer is the Alt-visible ground label, unlike the SoE V2
    // hidden-hover relay above. Both take the current quantity from the same
    // borrowed TYPE_ITEM and add the prefix before native text measurement.
    std::array<char,InnerNameBufferBytes> countedName{};
    if (mode == GeometryMode::Rules) {
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
    if (replacementBytes > capacity ||
        replacementBytes > RecordBytes - CandidateTextOffset) {
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
    if (mode == GeometryMode::Rules)
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

const char* GeometryModeText(GeometryMode mode) noexcept {
    switch (mode) {
        case GeometryMode::Observe: return "observe";
        case GeometryMode::Long: return "long";
        case GeometryMode::Short: return "short";
        case GeometryMode::Rules: return "rules";
        default: return "off";
    }
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
        TraceLootLatency(code,recordId,LootLatencyStage::MatchBulk,
            "formatter-ground-identity-match",0,scalars.quality,scalars.itemLevel);
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
    ObserveHoverRoute(HoverStage::Painter,caller,nullptr,textArg,rect,0,colorArg);
    ObserveHoverDifferentialDraw(HoverDiffRenderer::Painter,caller);
    if (const auto* scope=InWorldRenderScopeApi.load(std::memory_order_acquire);
        scope && ActiveGeometryMode.load(std::memory_order_relaxed)
                   == GeometryMode::Rules) {
        SoE::Interop::InWorldActiveItemV3 active{};
        active.structSize=sizeof(active);
        if (scope->getCurrentItem(&active))
            InWorldScopePainterCalls.fetch_add(1,std::memory_order_relaxed);
    }
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
    bool latencyBulkPaintQualified=false;
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
                        // Only write the exact native Divine color or the probe's
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
            latencyBulkPaintQualified=true;
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
        if (latencyBulkPaintQualified)
            TraceLootLatency(latencyBulkCode,latencyBulkRecordId,
                LootLatencyStage::BulkPaint,latencyBulkBackgroundForwarded ?
                "native-bulk-bg-forwarded":"native-bulk-text-forwarded");
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

// 0.1.99: observe the loader's public ItemTooltipEvent bus, used by SoE's
// Currency subsystem (see SoE currency/currency.cpp), rather than install
// another speculative hover-native detour. This is an *item tooltip event*
// probe: whether the bus publishes hidden ground hover is unknown and is
// precisely what our three isolated phases are intended to establish.
// No tooltip fragments are added and event->item is never modified.
constexpr ULONGLONG HoverEventWindowMs=12000;
constexpr std::uint32_t HoverEventMapCode=
    static_cast<std::uint32_t>('m') |
    (static_cast<std::uint32_t>('p')<<8U) |
    (static_cast<std::uint32_t>('0')<<16U) |
    (static_cast<std::uint32_t>('4')<<24U);
std::atomic<const D2RL::ItemServiceV1*> HoverEventItems{};
const D2RL::SharedEventServiceV1* HoverEventBus{};
D2RL::SharedEvents::ListenerHandle HoverEventListener{
    D2RL::SharedEvents::InvalidHandle};
std::atomic_bool HoverEventArmed{};
std::atomic<ULONGLONG> HoverEventDeadline{};
std::atomic<std::uint64_t> HoverEventTotal{};
std::atomic<std::uint64_t> HoverEventDivine{};
std::atomic<std::uint64_t> HoverEventMap{};
std::atomic<std::uint64_t> HoverEventOther{};
std::atomic<std::uint64_t> HoverEventInfoFailure{};
std::atomic<std::uint64_t> HoverEventBadEvent{};
std::atomic<std::uint64_t> HoverEventRuleMatches{};
std::atomic<std::uint32_t> HoverEventFirstCode{};
std::atomic<std::uint32_t> HoverEventLastCode{};
std::atomic<DWORD> HoverEventFirstThread{};
std::array<char,24> HoverEventPhase{};

void __cdecl OnHoverEvent(const D2RL::PluginContext* ctx,
    D2RL::SharedEvents::ItemTooltipEvent* event,void*) noexcept {
    if(!HoverEventArmed.load(std::memory_order_relaxed) ||
       GetTickCount64()>=HoverEventDeadline.load(std::memory_order_relaxed))
        return;
    if(!ctx || !event ||
       event->structSize < D2RL::SharedEvents::ItemTooltipEventRequiredSize) {
        HoverEventBadEvent.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    HoverEventTotal.fetch_add(1,std::memory_order_relaxed);
    DWORD expected{};
    (void)HoverEventFirstThread.compare_exchange_strong(
        expected,GetCurrentThreadId(),std::memory_order_relaxed);
    const auto* items=HoverEventItems.load(std::memory_order_acquire);
    if(!items || !items->getItemInfo) {
        HoverEventInfoFailure.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    D2RL::Items::ItemInfo info{};
    info.structSize=D2RL::Items::ItemInfoSize;
    if(items->getItemInfo(ctx,event->item,&info)
       !=D2RL::Items::Result::Success) {
        HoverEventInfoFailure.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    std::uint32_t empty{};
    (void)HoverEventFirstCode.compare_exchange_strong(
        empty,info.code,std::memory_order_relaxed);
    HoverEventLastCode.store(info.code,std::memory_order_relaxed);
    if(info.code==DivineCode)
        HoverEventDivine.fetch_add(1,std::memory_order_relaxed);
    else if(info.code==HoverEventMapCode)
        HoverEventMap.fetch_add(1,std::memory_order_relaxed);
    else
        HoverEventOther.fetch_add(1,std::memory_order_relaxed);
    // PublishedFilterRules is immutable; do not alter the event or send a UI
    // notification from a frequently invoked SDK callback.
    const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if(rules) {
        RuleEngine::Item item{};item.code=CanonicalItemCode(info.code);
        item.classIdKnown=true;item.classId=info.classId;
        item.quantityKnown=true;item.quantity=info.quantity>1?
            static_cast<std::uint32_t>(info.quantity):1U;
        GroundRuleDecision resolvedRule{};
        if(ResolveGroundRule(rules.get(),item,resolvedRule))
            HoverEventRuleMatches.fetch_add(1,std::memory_order_relaxed);
    }
}

void ArmHoverEventObserver() noexcept {
    if(HoverEventListener!=D2RL::SharedEvents::InvalidHandle) {
        Emit("LOOT_HOVER_EVENT_READY version=1.0.0 already-registered=1 public-SDK=1 modifications=0");
        return;
    }
    if(!Context || !D2RL::GetBuildName(Context) ||
       std::string_view(D2RL::GetBuildName(Context))!="93847") {
        Emit("LOOT_HOVER_EVENT_REFUSED version=1.0.0 reason=unsupported-build-or-context");
        return;
    }
    const D2RL::ItemServiceV1* items{};
    const D2RL::SharedEventServiceV1* bus{};
    if(Context->QueryService(D2RL::ServiceId::Item,
           D2RL::ItemServiceV1Version,&items)!=D2RL::ServiceQueryResult::Success ||
       !D2RL::HasItemServiceV1Field(items,
           D2RL::ItemServiceV1RequiredSize) ||
       !items->getItemInfo ||
       Context->QueryService(D2RL::ServiceId::SharedEvent,
           D2RL::SharedEventServiceV1Version,&bus)!=D2RL::ServiceQueryResult::Success ||
       !bus || !D2RL::HasSharedEventServiceV1Field(bus,
           D2RL::SharedEventServiceV1RequiredSize) ||
       !bus->registerItemTooltipListener ||
       !bus->unregisterItemTooltipListener) {
        Emit("LOOT_HOVER_EVENT_REFUSED version=1.0.0 reason=public-item-or-shared-event-service-unavailable no-native-fallback=1");
        return;
    }
    const D2RL::SharedEvents::ItemTooltipListener listener{
        .structSize=D2RL::SharedEvents::ItemTooltipListenerSize,
        .flags=0,
        .priority=100,
        .slot=0,
        .region=D2RL::SharedEvents::ItemTooltipRegion::ActionFooter,
        .position=D2RL::SharedEvents::ItemTooltipPosition::Bottom,
        .anchor=D2RL::SharedEvents::ItemTooltipAnchor::None,
        .fallback=D2RL::SharedEvents::ItemTooltipFallback::Omit,
        .callback=&OnHoverEvent,
        .userData=nullptr,
    };
    HoverEventItems.store(items,std::memory_order_release);HoverEventBus=bus;
    auto handle=D2RL::SharedEvents::InvalidHandle;
    const auto result=bus->registerItemTooltipListener(Context,&listener,&handle);
    if(result!=D2RL::SharedEvents::Result::Success ||
       handle==D2RL::SharedEvents::InvalidHandle) {
        HoverEventItems.store(nullptr,std::memory_order_release);HoverEventBus=nullptr;
        Emit("LOOT_HOVER_EVENT_REFUSED version=1.0.0 reason=public-listener-registration-failed no-hook-installed=1");
        return;
    }
    HoverEventListener=handle;
    Emit("LOOT_HOVER_EVENT_READY version=1.0.0 source=D2RLoader-SharedEvents.ItemTooltipEvent itemInfo=public-ItemService includesGroundHover=unverified readOnly=1 fragmentSubmission=none nativeHooksAdded=0 SoEImageHook=untouched");
}

void StartHoverEvent(std::string_view phase) noexcept {
    if(phase!="idle" && phase!="visible" && phase!="hidden" &&
       phase!="inventory") {
        Emit("LOOT_HOVER_EVENT_REFUSED usage: hover-event-start idle|visible|hidden|inventory");
        return;
    }
    if(HoverEventListener==D2RL::SharedEvents::InvalidHandle) {
        Emit("LOOT_HOVER_EVENT_REFUSED use-hover-event-observe-first");
        return;
    }
    HoverEventArmed.store(false,std::memory_order_release);
    HoverEventTotal.store(0);HoverEventDivine.store(0);HoverEventMap.store(0);
    HoverEventOther.store(0);HoverEventInfoFailure.store(0);
    HoverEventBadEvent.store(0);HoverEventRuleMatches.store(0);
    HoverEventFirstCode.store(0);HoverEventLastCode.store(0);
    HoverEventFirstThread.store(0);HoverEventPhase.fill(0);
    std::memcpy(HoverEventPhase.data(),phase.data(),phase.size());
    HoverEventDeadline.store(GetTickCount64()+HoverEventWindowMs,
        std::memory_order_release);
    HoverEventArmed.store(true,std::memory_order_release);
    char msg[260]{};
    std::snprintf(msg,sizeof(msg),
        "LOOT_HOVER_EVENT_ARMED version=1.0.0 phase='%s' durationMs=%llu source=public-ItemTooltipEvent nativeHooksAdded=0",
        HoverEventPhase.data(),
        static_cast<unsigned long long>(HoverEventWindowMs));
    Emit(msg);
}

void ReportHoverEvent(bool stop) noexcept {
    if(stop) HoverEventArmed.store(false,std::memory_order_release);
    char msg[590]{};
    std::snprintf(msg,sizeof(msg),
        "LOOT_HOVER_EVENT_PHASE version=1.0.0 phase='%s' registered=%u armed=%u events=%llu divo=%llu mp04=%llu other=%llu getInfoFailures=%llu invalidEvents=%llu ruleHits=%llu firstCode=0x%08X lastCode=0x%08X firstTid=%lu source=public-ItemTooltipEvent ground-vs-inventory-meaning=phase-dependent no-event-does-not-prove-no-hover=1 writes=0",
        HoverEventPhase.data(),
        HoverEventListener!=D2RL::SharedEvents::InvalidHandle?1U:0U,
        HoverEventArmed.load()?1U:0U,
        static_cast<unsigned long long>(HoverEventTotal.load()),
        static_cast<unsigned long long>(HoverEventDivine.load()),
        static_cast<unsigned long long>(HoverEventMap.load()),
        static_cast<unsigned long long>(HoverEventOther.load()),
        static_cast<unsigned long long>(HoverEventInfoFailure.load()),
        static_cast<unsigned long long>(HoverEventBadEvent.load()),
        static_cast<unsigned long long>(HoverEventRuleMatches.load()),
        HoverEventFirstCode.load(),HoverEventLastCode.load(),
        static_cast<unsigned long>(HoverEventFirstThread.load()));
    Emit(msg);
    FlushCapture();
}

void ReleaseHoverEventObserver() noexcept {
    HoverEventArmed.store(false,std::memory_order_release);
    if(HoverEventBus && Context &&
       HoverEventListener!=D2RL::SharedEvents::InvalidHandle)
        (void)HoverEventBus->unregisterItemTooltipListener(
            Context,HoverEventListener);
    HoverEventListener=D2RL::SharedEvents::InvalidHandle;
    HoverEventBus=nullptr;
    HoverEventItems.store(nullptr,std::memory_order_release);
}

// 0.1.99: phase-isolated, read-only tracing of the existing ABI-qualified
// glyph-B callback. This is a text-renderer candidate, NOT an item-tooltip
// hook and the RGBA argument has no relation to an item identity. No new
// detour: we reuse 0x658510; 0x858510 is SoE currency ImageWidget submit.
constexpr ULONGLONG UiTextCaptureMs=12000;
constexpr std::size_t UiTextMaxSites=16;
constexpr std::uint64_t UiTextSampleInterval=128;
struct UiTextSite {
    std::uintptr_t returnRva{};
    std::uint64_t sampled{};
    DWORD firstTid{};
    float firstX{};
    float firstY{};
    bool colorPointerPresent{};
};
std::atomic_bool UiTextArmed{};
std::atomic<ULONGLONG> UiTextDeadline{};
std::atomic<std::uint64_t> UiTextCalls{};
std::atomic<std::uint64_t> UiTextKnownReturnCalls{};
std::atomic<std::uint64_t> UiTextOtherReturnCalls{};
std::atomic<std::uint64_t> UiTextContended{};
std::atomic<std::uint64_t> UiTextOverflow{};
std::array<char,24> UiTextPhase{};
std::array<UiTextSite,UiTextMaxSites> UiTextSites{};
std::size_t UiTextSiteCount{};
std::mutex UiTextMutex;

void ObserveUiTextGlyphB(std::uintptr_t caller,float x,float y,
                         const float* color) noexcept {
    if(!UiTextArmed.load(std::memory_order_relaxed) ||
       GetTickCount64()>=UiTextDeadline.load(std::memory_order_relaxed))
        return;
    const auto hits=UiTextCalls.fetch_add(1,std::memory_order_relaxed)+1;
    const auto rva=caller>=Base && caller-Base<ImageSize?caller-Base:0;
    if(rva==CorrectedGlyphBCallRva+5)
        UiTextKnownReturnCalls.fetch_add(1,std::memory_order_relaxed);
    else
        UiTextOtherReturnCalls.fetch_add(1,std::memory_order_relaxed);
    // One in 128 calls records a caller/position tuple. All glyph-B calls
    // are counted, but site counts are sampled and must not be treated as
    // exact frequencies. Never dereference glyph context or the color pointer.
    if((hits%UiTextSampleInterval)!=0) return;
    if(!UiTextMutex.try_lock()) {
        UiTextContended.fetch_add(1,std::memory_order_relaxed);return;
    }
    {
        std::lock_guard<std::mutex> guard(UiTextMutex,std::adopt_lock);
        if(!UiTextArmed.load(std::memory_order_relaxed)) return;
        UiTextSite* row{};
        for(std::size_t i=0;i<UiTextSiteCount;++i)
            if(UiTextSites[i].returnRva==rva) {row=&UiTextSites[i];break;}
        if(!row) {
            if(UiTextSiteCount>=UiTextMaxSites) {
                UiTextOverflow.fetch_add(1,std::memory_order_relaxed);return;
            }
            row=&UiTextSites[UiTextSiteCount++];
            *row={};row->returnRva=rva;row->firstTid=GetCurrentThreadId();
            row->firstX=x;row->firstY=y;row->colorPointerPresent=color!=nullptr;
        }
        ++row->sampled;
    }
}

// 0.1.99: group *caller ancestry* of a sampled subset of already qualified
// glyph-B draw calls. 0.1.53 established that the direct return site is always
// D2R+0x908580 in every test; this observer looks at the next stack frames
// rather than assigning an item identity to an individual glyph. It does not
// dereference context/color pointers, mutate text or install another detour.
// Sampling is 1/256 and a bounded synchronous stack walk is deliberately done
// *outside* the results lock. Counters are relative and are NOT an item ID.
constexpr ULONGLONG HoverStackDurationMs=12'000;
constexpr std::uint64_t HoverStackSampling=256;
constexpr std::size_t HoverStackMaxSites=128;
constexpr std::size_t HoverStackTraceFrames=20;
constexpr std::size_t HoverStackKeyDepth=7;
struct HoverStackSite {
    std::array<std::uint32_t,HoverStackKeyDepth> d2rFrames{};
    std::uint64_t samples{};
    DWORD firstTid{};
    float firstX{};
    float firstY{};
};
std::atomic_bool HoverStackArmed{};
std::atomic<ULONGLONG> HoverStackDeadline{};
std::atomic<std::uint64_t> HoverStackCalls{};
std::atomic<std::uint64_t> HoverStackSampled{};
std::atomic<std::uint64_t> HoverStackWrongCaller{};
std::atomic<std::uint64_t> HoverStackNoD2r{};
std::atomic<std::uint64_t> HoverStackOverflow{};
std::atomic<std::uint64_t> HoverStackContended{};
std::array<char,24> HoverStackPhase{};
std::array<HoverStackSite,HoverStackMaxSites> HoverStackSites{};
std::size_t HoverStackSiteCount{};
std::mutex HoverStackMutex;

__declspec(noinline) void ObserveHoverGlyphStack(std::uintptr_t caller,
                                                  float x,float y) noexcept {
    if(!HoverStackArmed.load(std::memory_order_relaxed) ||
       GetTickCount64()>=HoverStackDeadline.load(std::memory_order_relaxed))
        return;
    const auto calls=HoverStackCalls.fetch_add(1,std::memory_order_relaxed)+1;
    if(caller!=Base+CorrectedGlyphBCallRva+5) {
        HoverStackWrongCaller.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    if(calls%HoverStackSampling!=0) return;
    std::array<void*,HoverStackTraceFrames> stack{};
    const USHORT depth=RtlCaptureStackBackTrace(0,
        static_cast<ULONG>(stack.size()),stack.data(),nullptr);
    std::array<std::uint32_t,HoverStackKeyDepth> key{};
    std::size_t count{};
    // A native stack frame is not a proof that a glyph belongs to a particular
    // item. Preserve the ordered D2R ancestry for *differential* inspection.
    for(USHORT i=0;i<depth && count<key.size();++i) {
        const auto address=reinterpret_cast<std::uintptr_t>(stack[i]);
        if(address<Base || address-Base>=ImageSize) continue;
        const auto rva=address-Base;
        if(rva>UINT32_MAX) continue;
        key[count++]=static_cast<std::uint32_t>(rva);
    }
    HoverStackSampled.fetch_add(1,std::memory_order_relaxed);
    if(!count) {HoverStackNoD2r.fetch_add(1,std::memory_order_relaxed);return;}
    if(!HoverStackMutex.try_lock()) {
        HoverStackContended.fetch_add(1,std::memory_order_relaxed);return;
    }
    std::lock_guard guard(HoverStackMutex,std::adopt_lock);
    if(!HoverStackArmed.load(std::memory_order_relaxed)) return;
    for(std::size_t i=0;i<HoverStackSiteCount;++i) {
        if(HoverStackSites[i].d2rFrames==key) {
            ++HoverStackSites[i].samples;return;
        }
    }
    if(HoverStackSiteCount>=HoverStackSites.size()) {
        HoverStackOverflow.fetch_add(1,std::memory_order_relaxed);return;
    }
    auto& row=HoverStackSites[HoverStackSiteCount++];
    row={};row.d2rFrames=key;row.samples=1;
    row.firstTid=GetCurrentThreadId();row.firstX=x;row.firstY=y;
}

void ReportHoverStack(bool stop) noexcept {
    if(stop) HoverStackArmed.store(false,std::memory_order_release);
    std::lock_guard lock(HoverStackMutex);
    char msg[850]{};
    std::snprintf(msg,sizeof(msg),
        "LOOT_HOVER_STACK_BEGIN version=1.0.0 phase='%s' active=%u calls=%llu sampled=%llu differentDirectCaller=%llu noD2rFrames=%llu rows=%zu overflow=%llu contention=%llu interval=%llu maxFrames=%zu scope=glyph-B-ancestry-only identity=unknown writes=0",
        HoverStackPhase.data(),HoverStackArmed.load()?1U:0U,
        static_cast<unsigned long long>(HoverStackCalls.load()),
        static_cast<unsigned long long>(HoverStackSampled.load()),
        static_cast<unsigned long long>(HoverStackWrongCaller.load()),
        static_cast<unsigned long long>(HoverStackNoD2r.load()),
        HoverStackSiteCount,
        static_cast<unsigned long long>(HoverStackOverflow.load()),
        static_cast<unsigned long long>(HoverStackContended.load()),
        static_cast<unsigned long long>(HoverStackSampling),HoverStackTraceFrames);
    Emit(msg);
    std::array<std::size_t,HoverStackMaxSites> order{};
    for(std::size_t i=0;i<HoverStackSiteCount;++i)order[i]=i;
    std::sort(order.begin(),order.begin()+HoverStackSiteCount,
        [](std::size_t a,std::size_t b) {
            return HoverStackSites[a].samples>HoverStackSites[b].samples;
        });
    for(std::size_t k=0;k<HoverStackSiteCount;++k) {
        const auto& row=HoverStackSites[order[k]];
        std::snprintf(msg,sizeof(msg),
            "LOOT_HOVER_STACK_SITE phase='%s' rank=%zu samples=%llu tid=%lu firstX=%.2f firstY=%.2f frames=D2R+0x%X,D2R+0x%X,D2R+0x%X,D2R+0x%X,D2R+0x%X,D2R+0x%X,D2R+0x%X sampleOnly=1 itemCode=unavailable",
            HoverStackPhase.data(),k+1,
            static_cast<unsigned long long>(row.samples),
            static_cast<unsigned long>(row.firstTid),row.firstX,row.firstY,
            row.d2rFrames[0],row.d2rFrames[1],row.d2rFrames[2],
            row.d2rFrames[3],row.d2rFrames[4],row.d2rFrames[5],
            row.d2rFrames[6]);
        CaptureLine(msg); // inactive legacy sampling; no output
    }
    Emit("LOOT_HOVER_STACK_END compare-ancestry-signatures-across-isolated-phases; no-ground-item-identity-yet; no-new-native-detours");
    FlushCapture();
}

void StartHoverStack(std::string_view phase) noexcept {
    if(phase!="idle" && phase!="visible" && phase!="hidden" &&
       phase!="inventory" && phase!="map") {
        Emit("LOOT_HOVER_STACK_REFUSED usage: hover-stack-start idle|visible|hidden|inventory|map");return;
    }
    if(!CorrectedGlyphBInstalled.load(std::memory_order_acquire) ||
       !OriginalCorrectedGlyphB ||
       CorrectedGlyphBArmed.load(std::memory_order_acquire) ||
       UiTextArmed.load(std::memory_order_acquire) ||
       ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Off ||
       RenameArmed.load(std::memory_order_acquire) ||
       CodeRenameArmed.load(std::memory_order_acquire) ||
       BackgroundTintArmed.load(std::memory_order_acquire) ||
       GroundTextCyanArmed.load(std::memory_order_acquire)) {
        Emit("LOOT_HOVER_STACK_REFUSED observer-not-installed-or-other-visual-mode-active; no-state-changed");return;
    }
    HoverStackArmed.store(false,std::memory_order_release);
    {
        std::lock_guard lock(HoverStackMutex);
        HoverStackPhase.fill(0);
        std::memcpy(HoverStackPhase.data(),phase.data(),phase.size());
        HoverStackSites.fill({});HoverStackSiteCount=0;
        HoverStackCalls.store(0);HoverStackSampled.store(0);
        HoverStackWrongCaller.store(0);HoverStackNoD2r.store(0);
        HoverStackOverflow.store(0);HoverStackContended.store(0);
        HoverStackDeadline.store(GetTickCount64()+HoverStackDurationMs,
            std::memory_order_release);
        HoverStackArmed.store(true,std::memory_order_release);
    }
    char msg[460]{};
    std::snprintf(msg,sizeof(msg),"LOOT_HOVER_STACK_ARMED version=1.0.0 phase='%s' durationMs=%llu nativeTarget=D2R+0x658510 knownReturn=0x908580 readOnly=1 sampling=1/%llu newSpeculativeHooks=0",
        HoverStackPhase.data(),static_cast<unsigned long long>(HoverStackDurationMs),
        static_cast<unsigned long long>(HoverStackSampling));
    Emit(msg);
}

// Forward-declare the already-defined glyph-B installer: the 0.1.71
// hover-stack command calls it before its full definition below.
void ArmCorrectedGlyphB(bool observeOnly) noexcept;

void ArmHoverStack() noexcept {
    if(CorrectedGlyphBArmed.load(std::memory_order_acquire) ||
       ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Off ||
       RenameArmed.load(std::memory_order_acquire) ||
       CodeRenameArmed.load(std::memory_order_acquire) ||
       BackgroundTintArmed.load(std::memory_order_acquire) ||
       GroundTextCyanArmed.load(std::memory_order_acquire)) {
        Emit("LOOT_HOVER_STACK_REFUSED active-visual-modification; do-not-disarm-user-filter-automatically");return;
    }
    ArmCorrectedGlyphB(true); // already ABI-qualified by 0.1.42/0.1.53
    if(!CorrectedGlyphBInstalled.load(std::memory_order_acquire) ||
       !OriginalCorrectedGlyphB ||
       CorrectedGlyphBArmed.load(std::memory_order_acquire)) {
        Emit("LOOT_HOVER_STACK_REFUSED glyph-B-hook-not-qualified-or-color-mode-active");return;
    }
    Emit("LOOT_HOVER_STACK_READY version=1.0.0 source=existing-ABI-qualified-glyph-B target=D2R+0x658510 directCall=D2R+0x90857B stackSamples=bounded readOnly=1 newSpeculativeHooks=0 imageWidgetRva=0x858510-untouched");
}

// 0.1.99: opt-in, bounded glyph-coordinate differential. 0.1.58 established
// that native glyph-B's return/stack signature is identical across UI states.
// This is a spatial CONTROL experiment, not an item-code/hover renderer claim.
// No pointer dereference, code write, or extra native detour is performed here.
// Captured coordinates are native UI coordinates; do not assume physical pixels.
constexpr ULONGLONG HoverSpatialDurationMs=12'000;
constexpr std::uint64_t HoverSpatialStride=32;
constexpr std::size_t HoverSpatialMaxRows=2048;
constexpr std::size_t HoverSpatialDumpRows=96;
constexpr float HoverSpatialTileSize=32.0f;
struct HoverSpatialRow {
    std::int32_t binX{},binY{};
    std::uint64_t hits{};
    float exampleX{},exampleY{};
    std::uint32_t threadId{};
};
std::atomic_bool HoverSpatialArmed{};
std::atomic<ULONGLONG> HoverSpatialDeadline{};
std::atomic<std::uint64_t> HoverSpatialCalls{};
std::atomic<std::uint64_t> HoverSpatialSamples{};
std::atomic<std::uint64_t> HoverSpatialInvalid{};
std::atomic<std::uint64_t> HoverSpatialOverflow{};
std::atomic<std::uint64_t> HoverSpatialContention{};
std::atomic<std::uint64_t> HoverSpatialWrongCaller{};
std::array<char,24> HoverSpatialPhase{};
std::array<HoverSpatialRow,HoverSpatialMaxRows> HoverSpatialRows{};
std::size_t HoverSpatialRowCount{};
std::mutex HoverSpatialMutex;

void ObserveHoverSpatial(std::uintptr_t caller,float x,float y) noexcept {
    if(!HoverSpatialArmed.load(std::memory_order_relaxed) ||
       GetTickCount64()>=HoverSpatialDeadline.load(std::memory_order_relaxed))return;
    const auto n=HoverSpatialCalls.fetch_add(1,std::memory_order_relaxed)+1;
    if(caller!=Base+CorrectedGlyphBCallRva+5) {
        HoverSpatialWrongCaller.fetch_add(1,std::memory_order_relaxed);return;
    }
    if(n%HoverSpatialStride!=0)return;
    if(!std::isfinite(x) || !std::isfinite(y) ||
       x < -1'000'000.0f || x > 1'000'000.0f ||
       y < -1'000'000.0f || y > 1'000'000.0f) {
        HoverSpatialInvalid.fetch_add(1,std::memory_order_relaxed);return;
    }
    const auto bx=static_cast<std::int32_t>(std::floor(x/HoverSpatialTileSize));
    const auto by=static_cast<std::int32_t>(std::floor(y/HoverSpatialTileSize));
    HoverSpatialSamples.fetch_add(1,std::memory_order_relaxed);
    if(!HoverSpatialMutex.try_lock()) {
        HoverSpatialContention.fetch_add(1,std::memory_order_relaxed);return;
    }
    std::lock_guard lock(HoverSpatialMutex,std::adopt_lock);
    if(!HoverSpatialArmed.load(std::memory_order_relaxed))return;
    for(std::size_t i=0;i<HoverSpatialRowCount;++i) {
        auto& row=HoverSpatialRows[i];
        if(row.binX==bx && row.binY==by) {++row.hits;return;}
    }
    if(HoverSpatialRowCount==HoverSpatialRows.size()) {
        HoverSpatialOverflow.fetch_add(1,std::memory_order_relaxed);return;
    }
    auto& row=HoverSpatialRows[HoverSpatialRowCount++];
    row={bx,by,1,x,y,GetCurrentThreadId()};
}

void ArmHoverSpatial() noexcept {
    if(CorrectedGlyphBArmed.load(std::memory_order_acquire) ||
       RenameArmed.load(std::memory_order_acquire) ||
       CodeRenameArmed.load(std::memory_order_acquire) ||
       BackgroundTintArmed.load(std::memory_order_acquire) ||
       GroundTextCyanArmed.load(std::memory_order_acquire)) {
        Emit("LOOT_HOVER_SPATIAL_REFUSED visual-modification-active do-not-change-user-filter");return;
    }
    ArmCorrectedGlyphB(true);
    if(!CorrectedGlyphBInstalled.load(std::memory_order_acquire) ||
       !OriginalCorrectedGlyphB ||
       CorrectedGlyphBArmed.load(std::memory_order_acquire)) {
        Emit("LOOT_HOVER_SPATIAL_REFUSED glyph-B-observer-unavailable no-new-hook");return;
    }
    Emit("LOOT_HOVER_SPATIAL_READY version=1.0.0 readOnly=1 knownGlyphB=D2R+0x658510 knownCall=D2R+0x90857B newHooks=0 imageWidget=untouched UI-coordinates-not-physical-pixels identity=unknown");
}

void StartHoverSpatial(std::string_view phase) noexcept {
    if(phase!="away-before" && phase!="on" &&
       phase!="away-after" && phase!="map-on") {
        Emit("LOOT_HOVER_SPATIAL_REFUSED usage:hover-spatial-start away-before|on|away-after|map-on");return;
    }
    if(!CorrectedGlyphBInstalled.load(std::memory_order_acquire) ||
       !OriginalCorrectedGlyphB ||
       CorrectedGlyphBArmed.load(std::memory_order_acquire)) {
        Emit("LOOT_HOVER_SPATIAL_REFUSED install-hover-spatial-observe-first or visual-mode-active");return;
    }
    HoverSpatialArmed.store(false,std::memory_order_release);
    {
        std::lock_guard lock(HoverSpatialMutex);
        HoverSpatialRows.fill({}); HoverSpatialRowCount=0;
        HoverSpatialCalls.store(0);HoverSpatialSamples.store(0);
        HoverSpatialInvalid.store(0);HoverSpatialOverflow.store(0);
        HoverSpatialContention.store(0);HoverSpatialWrongCaller.store(0);
        HoverSpatialPhase.fill(0);
        std::memcpy(HoverSpatialPhase.data(),phase.data(),phase.size());
        HoverSpatialDeadline.store(GetTickCount64()+HoverSpatialDurationMs,
                                   std::memory_order_release);
        HoverSpatialArmed.store(true,std::memory_order_release);
    }
    char msg[300]{};
    std::snprintf(msg,sizeof(msg),
        "LOOT_HOVER_SPATIAL_ARMED version=1.0.0 phase='%s' durationMs=%llu stride=%llu tile=%.0f nativeUIUnits readOnly=1",
        HoverSpatialPhase.data(),
        static_cast<unsigned long long>(HoverSpatialDurationMs),
        static_cast<unsigned long long>(HoverSpatialStride),
        static_cast<double>(HoverSpatialTileSize));
    Emit(msg);
}

void ReportHoverSpatial(bool stop) noexcept {
    if(stop)HoverSpatialArmed.store(false,std::memory_order_release);
    // Snapshot under lock, emit only on the console/control thread.
    std::array<HoverSpatialRow,HoverSpatialMaxRows> rows{};
    std::array<char,24> phase{};
    std::size_t count{};
    {
        std::lock_guard lock(HoverSpatialMutex);
        count=HoverSpatialRowCount;
        std::copy_n(HoverSpatialRows.begin(),count,rows.begin());
        phase=HoverSpatialPhase;
    }
    std::sort(rows.begin(),rows.begin()+count,
        [](const HoverSpatialRow& a,const HoverSpatialRow& b){
            return a.hits>b.hits;
        });
    char msg[470]{};
    std::snprintf(msg,sizeof(msg),
        "LOOT_HOVER_SPATIAL_BEGIN version=1.0.0 phase='%s' active=%u calls=%llu sampled=%llu differentDirectCaller=%llu invalidCoords=%llu bins=%zu printed=%zu overflow=%llu contended=%llu stride=%llu tile=%.0f coordinateUnits=nativeUI identity=unknown",
        phase.data(),HoverSpatialArmed.load()?1U:0U,
        static_cast<unsigned long long>(HoverSpatialCalls.load()),
        static_cast<unsigned long long>(HoverSpatialSamples.load()),
        static_cast<unsigned long long>(HoverSpatialWrongCaller.load()),
        static_cast<unsigned long long>(HoverSpatialInvalid.load()),
        count,std::min(count,HoverSpatialDumpRows),
        static_cast<unsigned long long>(HoverSpatialOverflow.load()),
        static_cast<unsigned long long>(HoverSpatialContention.load()),
        static_cast<unsigned long long>(HoverSpatialStride),
        static_cast<double>(HoverSpatialTileSize));
    Emit(msg);
    for(std::size_t i=0;i<std::min(count,HoverSpatialDumpRows);++i) {
        const auto& row=rows[i];
        std::snprintf(msg,sizeof(msg),
            "LOOT_HOVER_SPATIAL_BIN phase='%s' rank=%zu binX=%d binY=%d hits=%llu sampleX=%.2f sampleY=%.2f tid=%lu itemCode=unknown",
            phase.data(),i+1,static_cast<int>(row.binX),
            static_cast<int>(row.binY),
            static_cast<unsigned long long>(row.hits),
            static_cast<double>(row.exampleX),
            static_cast<double>(row.exampleY),
            static_cast<unsigned long>(row.threadId));
        Emit(msg);
    }
    Emit("LOOT_HOVER_SPATIAL_END compare-away-before/on/away-after normalized-bin-hits; name-content-and-selection-pointer-NOT-captured; no-item-writes no-hook-adds");
    FlushCapture();
}

// Diagnostic second-pass color trial. Trained on one item and one horizontal
// cluster in this game only. No pointer mutation; the array's lifetime spans
// exactly the synchronous original glyph-B call. Do not call this on a shared
// renderer without the item heartbeat, geometry and native-white guards.
// This is NOT sufficient evidence for general/public release hover RGB.
bool TryForwardHoverRgbGlyph(void* context, float x, float y,
    const float* nativeRgba, std::uint64_t& result) noexcept {
    if (!HoverRgbTrialEnabled ||
        !HoverRgbTrialReady.load(std::memory_order_acquire) ||
        !InWorldStyleAttached.load(std::memory_order_relaxed) ||
        ActiveGeometryMode.load(std::memory_order_relaxed)!=GeometryMode::Rules ||
        !OriginalCorrectedGlyphB || !nativeRgba ||
        VerifiedRuleGlyphColor || // existing Alt-visible recolor has precedence
        !std::isfinite(x) || !std::isfinite(y)) return false;
    const auto bx=static_cast<std::int32_t>(std::floor(x/HoverDiffTileSize));
    const auto by=static_cast<std::int32_t>(std::floor(y/HoverDiffTileSize));
    if (by!=HoverRgbTrialRowY.load(std::memory_order_relaxed) ||
        bx<HoverRgbTrialMinX.load(std::memory_order_relaxed) ||
        bx>HoverRgbTrialMaxX.load(std::memory_order_relaxed)) return false;
    HoverRgbTrialRegionHits.fetch_add(1,std::memory_order_relaxed);
    const auto expectedId=HoverRgbTrialItemId.load(std::memory_order_relaxed);
    const auto expectedCode=HoverRgbTrialCode.load(std::memory_order_relaxed);
    if (!expectedId || !expectedCode ||
        expectedId!=HoverDiffItemId.load(std::memory_order_acquire) ||
        expectedCode!=HoverDiffCode.load(std::memory_order_acquire) ||
        HoverDiffOtherItemCallbacks.load(std::memory_order_relaxed)!=0U) {
        HoverRgbTrialWrongItem.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    const auto now=GetTickCount64();
    const auto last=HoverDiffLastMatchingMs.load(std::memory_order_acquire);
    if (!last || now<last || now-last>100U) {
        HoverRgbTrialNotFresh.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    std::array<float,4> native{};
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(),nativeRgba,native.data(),
            sizeof(native),&copied) || copied!=sizeof(native)) {
        HoverRgbTrialRejectedColor.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    // Captured candidate glyphs were native 0.941,0.941,0.941,1.000.
    // Reject colored/transparent UI in the same region, fail closed.
    for (int i=0;i<3;++i) {
        if (!std::isfinite(native[i]) || std::fabs(native[i]-0.941f)>0.03f) {
            HoverRgbTrialRejectedColor.fetch_add(1,std::memory_order_relaxed);
            return false;
        }
    }
    if (!std::isfinite(native[3]) || native[3]<0.99f || native[3]>1.001f) {
        HoverRgbTrialRejectedColor.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if (rules) for (const auto& rule:rules->rules) {
        if (rule.code!=expectedCode || !rule.hasTextColor) continue;
        for (const auto channel:rule.textColor)
            if (!std::isfinite(channel) || channel<0.f || channel>1.f) {
                HoverRgbTrialRejectedColor.fetch_add(1,
                    std::memory_order_relaxed);
                return false;
            }
        std::array<float,4> rgba=rule.textColor;
        // Never override engine-provided alpha with a different value in this
        // proof. The JSON RGB is what we are qualifying, not transparency.
        rgba[3]=native[3];
        HoverRgbTrialForwarded.fetch_add(1,std::memory_order_relaxed);
        result=OriginalCorrectedGlyphB(context,x,y,rgba.data());
        return true;
    }
    HoverRgbTrialNoRule.fetch_add(1,std::memory_order_relaxed);
    return false;
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

void StartUiTextProbe(std::string_view phase) noexcept {
    if(phase!="idle" && phase!="hidden" && phase!="inventory" &&
       phase!="visible") {
        Emit("LOOT_UI_TEXT_REFUSED usage: ui-text-start idle|hidden|inventory|visible");
        return;
    }
    if(!CorrectedGlyphBInstalled.load(std::memory_order_acquire) ||
       !OriginalCorrectedGlyphB) {
        Emit("LOOT_UI_TEXT_REFUSED install-ui-text-observe-first");return;
    }
    UiTextArmed.store(false,std::memory_order_release);
    {
        std::lock_guard<std::mutex> guard(UiTextMutex);
        UiTextSites.fill({});UiTextSiteCount=0;
        UiTextCalls.store(0);UiTextKnownReturnCalls.store(0);
        UiTextOtherReturnCalls.store(0);UiTextContended.store(0);
        UiTextOverflow.store(0);UiTextPhase.fill(0);
        std::memcpy(UiTextPhase.data(),phase.data(),phase.size());
        UiTextDeadline.store(GetTickCount64()+UiTextCaptureMs,
                             std::memory_order_release);
        UiTextArmed.store(true,std::memory_order_release);
    }
    char msg[320]{};
    std::snprintf(msg,sizeof(msg),
        "LOOT_UI_TEXT_ARMED version=1.0.0 phase='%s' durationMs=%llu source=already-qualified-glyph-B no-new-hook=1 glyphA=untouched no-tooltip-writes=1",
        UiTextPhase.data(),static_cast<unsigned long long>(UiTextCaptureMs));
    Emit(msg);
}

void ReportUiTextProbe(bool stop) noexcept {
    if(stop) UiTextArmed.store(false,std::memory_order_release);
    std::lock_guard<std::mutex> guard(UiTextMutex);
    char msg[480]{};
    std::snprintf(msg,sizeof(msg),
        "LOOT_UI_TEXT_BEGIN version=1.0.0 phase='%s' installed=%u armed=%u calls=%llu knownReturn=0x908580 knownCalls=%llu otherCalls=%llu siteSamples=%zu sampleInterval=%llu contended=%llu overflow=%llu",
        UiTextPhase.data(),CorrectedGlyphBInstalled.load()?1U:0U,
        UiTextArmed.load()?1U:0U,
        static_cast<unsigned long long>(UiTextCalls.load()),
        static_cast<unsigned long long>(UiTextKnownReturnCalls.load()),
        static_cast<unsigned long long>(UiTextOtherReturnCalls.load()),
        UiTextSiteCount,
        static_cast<unsigned long long>(UiTextSampleInterval),
        static_cast<unsigned long long>(UiTextContended.load()),
        static_cast<unsigned long long>(UiTextOverflow.load()));
    Emit(msg);
    for(std::size_t i=0;i<UiTextSiteCount;++i) {
        const auto& row=UiTextSites[i];
        std::snprintf(msg,sizeof(msg),
            "LOOT_UI_TEXT_SITE phase='%s' returnRva=D2R+0x%llX sampled=%llu tid=%lu firstX=%.2f firstY=%.2f colorPointerPresent=%u itemIdentity=unknown",
            UiTextPhase.data(),
            static_cast<unsigned long long>(row.returnRva),
            static_cast<unsigned long long>(row.sampled),
            static_cast<unsigned long>(row.firstTid),
            static_cast<double>(row.firstX),static_cast<double>(row.firstY),
            row.colorPointerPresent?1U:0U);
        Emit(msg);
    }
    Emit("LOOT_UI_TEXT_END scope=glyphB-only; zero-calls-do-not-prove-no-tooltip; no-new-detours; no-item-or-tooltip-writes");
    FlushCapture();
}

// Forward declaration: SyncFilterTextColorState is defined before this
// existing hook installer; MSVC requires the declaration before first use.
// One automatic bounded report after the first hidden-hover V2 callback;
// JSON is already active. No console commands, no per-glyph logging.
void ResetInWorldScopeProbe() noexcept {
    InWorldScopeGlyphCalls.store(0,std::memory_order_release);
    InWorldScopePainterCalls.store(0,std::memory_order_release);
    InWorldScopeRgbaSamples.store(0,std::memory_order_release);
    InWorldScopeMatchingRule.store(0,std::memory_order_release);
    InWorldScopeLastId.store(0,std::memory_order_release);
    InWorldScopeLastCode.store(0,std::memory_order_release);
    InWorldScopeFirstHoverMs.store(0,std::memory_order_release);
    InWorldScopeReported.store(false,std::memory_order_release);
}
void ReportInWorldScopeProbe() noexcept {
    std::array<float,4> rgba{};
    { std::lock_guard lock(InWorldScopeSampleMutex);rgba=InWorldScopeLastRgba; }
    char line[650]{};
    const auto code=InWorldScopeLastCode.load(std::memory_order_acquire);
    const char chars[5]{char(code&0xff),char((code>>8)&0xff),
        char((code>>16)&0xff),char((code>>24)&0xff),0};
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_RENDER_SCOPE_RESULT version=1.0.0 hiddenCallbacks=%llu scopedGlyphB=%llu scopedGroundPainter=%llu sampledNativeRgba=%llu sampledJsonMatches=%llu lastItemId=%u lastCode='%.4s' lastNativeRgba=%.3f,%.3f,%.3f,%.3f interpretation=overlap-is-not-proof-of-glyph-ownership arbitraryColorWrites=0 backgroundWrites=0", 
        static_cast<unsigned long long>(InWorldStyleCalls.load()),
        static_cast<unsigned long long>(InWorldScopeGlyphCalls.load()),
        static_cast<unsigned long long>(InWorldScopePainterCalls.load()),
        static_cast<unsigned long long>(InWorldScopeRgbaSamples.load()),
        static_cast<unsigned long long>(InWorldScopeMatchingRule.load()),
        InWorldScopeLastId.load(),chars,
        double(rgba[0]),double(rgba[1]),double(rgba[2]),double(rgba[3]));
    Emit(line);
}

// 0.1.99: native rendering is not nested in the synchronous V3 label scope.
// Classify EXISTING renderer calls by proximity to V1 matching-label events.
// This is temporal correlation only, NOT ownership of a particular glyph.
HoverDiffPhase ClassifyHoverDifferential(ULONGLONG now) noexcept {
    return HoverDiffPolicy::Classify(now,
        HoverDiffBeginMs.load(std::memory_order_acquire),
        HoverDiffLastMatchingMs.load(std::memory_order_acquire),
        HoverDiffCompleted.load(std::memory_order_relaxed));
}

void ObserveHoverDifferentialDraw(HoverDiffRenderer renderer,
    std::uintptr_t caller, float x, float y,
    const float* rgba) noexcept {
    const auto now=GetTickCount64();
    const auto phase=ClassifyHoverDifferential(now);
    if (phase==HoverDiffPhase::None) return;
    auto& state=renderer==HoverDiffRenderer::Glyph ?
        HoverDiffGlyph : HoverDiffPainter;
    const auto number=state.observed.fetch_add(1,std::memory_order_relaxed)+1U;
    if (phase==HoverDiffPhase::Hover)
        state.hover.fetch_add(1,std::memory_order_relaxed);
    else if (phase==HoverDiffPhase::Away)
        state.away.fetch_add(1,std::memory_order_relaxed);
    else {
        state.transition.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    // Global counts are exact. 0.1.73 sampled every 16th glyph, which
    // phase-locked to a periodically repeating UI glyph sequence and saw only
    // ONE native XY tile even when a whole label was visible. Sample the
    // UNION of two relatively-prime periods: 17 and 19. This covers glyph
    // sequences whose per-frame lengths divide either period, without
    // doubling the native hook count or adding any native hooks. Exactly
    // 35 of each 323 successive calls pass (19+17-1 intersection).
    if ((number % 17U)!=1U && (number % 19U)!=1U) return;
    if (!state.mutex.try_lock()) {
        state.contention.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    const auto phaseIndex=phase==HoverDiffPhase::Hover?0U:1U;
    const auto inImage=caller>=Base && Base && caller-Base<ImageSize;
    const auto rva=inImage?caller-Base:0U;
    const auto thread=GetCurrentThreadId();
    auto* row=static_cast<HoverDiffSite*>(nullptr);
    for (std::size_t i=0;i<state.siteCount;++i) {
        if (state.sites[i].callerRva==rva &&
            state.sites[i].thread==thread) {
            row=&state.sites[i];break;
        }
    }
    if (!row && state.siteCount<state.sites.size()) {
        row=&state.sites[state.siteCount++];
        *row={};row->callerRva=rva;row->thread=thread;
        row->firstX=x;row->firstY=y;
    } else if (!row) state.overflow.fetch_add(1,std::memory_order_relaxed);
    // Shared renderer B is sampled at both 1/17 and 1/19 phases. Bin only those
    // samples, with fixed-size storage and no allocations or extra native hooks.
    if (renderer==HoverDiffRenderer::Glyph) {
        if (std::isfinite(x) && std::isfinite(y) &&
            x>=-4096.0f && x<=16384.0f &&
            y>=-4096.0f && y<=16384.0f) {
            const auto bx=static_cast<std::int32_t>(std::floor(x/HoverDiffTileSize));
            const auto by=static_cast<std::int32_t>(std::floor(y/HoverDiffTileSize));
            HoverDiffTile* tile=nullptr;
            for (std::size_t i=0;i<state.tileCount;++i) {
                if (state.tiles[i].binX==bx && state.tiles[i].binY==by) {
                    tile=&state.tiles[i];break;
                }
            }
            if (!tile && state.tileCount<state.tiles.size()) {
                tile=&state.tiles[state.tileCount++];
                *tile={};tile->binX=bx;tile->binY=by;
                tile->sampleX=x;tile->sampleY=y;
            }
            if (tile) {
                if (phaseIndex==0) {
                    ++tile->hover;
                    if (!tile->hoverRgbaValid && rgba) {
                        std::array<float,4> input{};
                        SIZE_T copied{};
                        if (ReadProcessMemory(GetCurrentProcess(),rgba,
                                input.data(),sizeof(input),&copied) &&
                            copied==sizeof(input)) {
                            bool valid=true;
                            for (const auto channel:input)
                                valid=valid && std::isfinite(channel) &&
                                    channel>=0.0f && channel<=1.001f;
                            if (valid) {
                                tile->hoverRgba=input;
                                tile->hoverRgbaValid=true;
                            }
                        }
                    }
                } else ++tile->away;
            } else ++state.tileOverflow;
        } else ++state.invalidTileCoords;
    }
    if (row) {
        if (phaseIndex==0)++row->hoverSamples;
        else ++row->awaySamples;
        if (!row->rgbaValid && rgba && renderer==HoverDiffRenderer::Glyph) {
            std::array<float,4> input{};
            SIZE_T count{};
            if (ReadProcessMemory(GetCurrentProcess(),rgba,input.data(),
                    sizeof(input),&count) && count==sizeof(input)) {
                bool valid=true;
                for (const float f:input)
                    valid=valid && std::isfinite(f) && f>=0.0f && f<=1.001f;
                if (valid) {row->firstRgba=input;row->rgbaValid=true;}
            }
        }
    }
    // At most two stack witnesses for each renderer/state, never a per-frame
    // stack scan. RtlCaptureStackBackTrace is read-only and has fixed storage.
    if (state.stackCounts[phaseIndex]<HoverDiffMaxStacks) {
        auto& stack=state.stacks[phaseIndex][state.stackCounts[phaseIndex]++];
        stack.thread=thread;stack.callerRva=rva;
        void* frames[HoverDiffStackFrames]{};
        const auto captured=CaptureStackBackTrace(1,
            static_cast<DWORD>(HoverDiffStackFrames), frames,nullptr);
        stack.count=static_cast<std::uint16_t>(captured);
        for (std::uint16_t i=0;i<stack.count;++i)
            stack.frames[i]=reinterpret_cast<std::uintptr_t>(frames[i]);
    }
    state.mutex.unlock();
}

void ResetHoverDifferentialProbe() noexcept {
    // Called at GameJoined while old writer may be finishing its report.
    HoverRgbTrialReady.store(false,std::memory_order_release);
    HoverRgbTrialItemId.store(0,std::memory_order_relaxed);
    HoverRgbTrialCode.store(0,std::memory_order_relaxed);
    HoverRgbTrialRegionHits.store(0,std::memory_order_relaxed);
    HoverRgbTrialForwarded.store(0,std::memory_order_relaxed);
    HoverRgbTrialRejectedColor.store(0,std::memory_order_relaxed);
    HoverRgbTrialNoRule.store(0,std::memory_order_relaxed);
    HoverRgbTrialNotFresh.store(0,std::memory_order_relaxed);
    HoverRgbTrialWrongItem.store(0,std::memory_order_relaxed);
    HoverDiffEpoch.fetch_add(1,std::memory_order_acq_rel);
    HoverDiffCompleted.store(false,std::memory_order_release);
    HoverDiffBeginMs.store(0,std::memory_order_release);
    HoverDiffLastMatchingMs.store(0,std::memory_order_release);
    HoverDiffMatches.store(0,std::memory_order_release);
    HoverDiffOtherItemCallbacks.store(0,std::memory_order_release);
    HoverDiffCode.store(0,std::memory_order_release);
    HoverDiffItemId.store(0,std::memory_order_release);
    for (auto* state : {&HoverDiffGlyph,&HoverDiffPainter}) {
        std::lock_guard lock(state->mutex);
        state->sites.fill({});state->siteCount=0;
        state->tiles.fill({});state->tileCount=0;
        state->invalidTileCoords=0;state->tileOverflow=0;
        state->stacks={};state->stackCounts={};
        state->observed.store(0,std::memory_order_relaxed);
        state->hover.store(0,std::memory_order_relaxed);
        state->away.store(0,std::memory_order_relaxed);
        state->transition.store(0,std::memory_order_relaxed);
        state->contention.store(0,std::memory_order_relaxed);
        state->overflow.store(0,std::memory_order_relaxed);
    }
}

void ReportHoverDifferentialRenderer(const char* name,
    HoverDiffRendererState& state,
    std::uint64_t hoverMs,std::uint64_t awayMs) noexcept {
    char line[600]{};
    const auto h=state.hover.load(std::memory_order_acquire);
    const auto a=state.away.load(std::memory_order_acquire);
    std::lock_guard lock(state.mutex);
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_DIFF_RENDERER version=1.0.0 renderer=%s hoverCalls=%llu awayCalls=%llu transitionCalls=%llu hoverMs=%llu awayMs=%llu hoverCallsPerSecond=%.2f awayCallsPerSecond=%.2f sitesSampled=%zu sampleMode=dual-17-19 sampleFraction=35/323 contention=%llu overflow=%llu",
        name,static_cast<unsigned long long>(h),
        static_cast<unsigned long long>(a),
        static_cast<unsigned long long>(state.transition.load()),
        static_cast<unsigned long long>(hoverMs),
        static_cast<unsigned long long>(awayMs),
        hoverMs?1000.0*double(h)/double(hoverMs):0.0,
        awayMs?1000.0*double(a)/double(awayMs):0.0,
        state.siteCount,
        static_cast<unsigned long long>(state.contention.load()),
        static_cast<unsigned long long>(state.overflow.load()));
    Emit(line);
    for (std::size_t i=0;i<state.siteCount;++i) {
        const auto& row=state.sites[i];
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_DIFF_SITE renderer=%s returnRva=D2R+0x%llX tid=%lu hoverSampled=%llu awaySampled=%llu hoverEstimatedHz=%.2f awayEstimatedHz=%.2f sampleMode=dual-17-19 sampleFraction=35/323 firstXY=%.2f,%.2f rgbaPresent=%u nativeRgba=%.3f,%.3f,%.3f,%.3f ownership=unproven",
            name,static_cast<unsigned long long>(row.callerRva),
            static_cast<unsigned long>(row.thread),
            static_cast<unsigned long long>(row.hoverSamples),
            static_cast<unsigned long long>(row.awaySamples),
            hoverMs?1000.0*(323.0/35.0)*double(row.hoverSamples)/double(hoverMs):0.0,
            awayMs?1000.0*(323.0/35.0)*double(row.awaySamples)/double(awayMs):0.0,
            double(row.firstX),double(row.firstY),row.rgbaValid?1U:0U,
            double(row.firstRgba[0]),double(row.firstRgba[1]),
            double(row.firstRgba[2]),double(row.firstRgba[3]));
        Emit(line);
    }
    if (&state==&HoverDiffGlyph) {
        // Sort local value copies while holding lock; never re-order live bins.
        auto sorted=state.tiles;
        const auto n=state.tileCount;
        std::sort(sorted.begin(),sorted.begin()+n,
            [hoverMs,awayMs](const HoverDiffTile& l,
                             const HoverDiffTile& r) noexcept {
                // Signed, duration-normalized excess, from the dual-period glyph sample.
                const auto excess=[hoverMs,awayMs](const HoverDiffTile& v) noexcept {
                    return std::int64_t(v.hover)*std::int64_t(awayMs)-
                           std::int64_t(v.away)*std::int64_t(hoverMs);
                };
                return excess(l)>excess(r);
            });
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_DIFF_SPATIAL version=1.0.0 renderer=glyph-B tileSize=32 nativeUIUnits totalTiles=%zu printed=%zu invalidCoords=%llu overflow=%llu sampleMode=dual-17-19 sampleFraction=35/323 topBy=duration-normalized-hover-minus-away readOnly=1 itemOwnership=unproven",
            n,std::min(n,HoverDiffPrintTiles),
            static_cast<unsigned long long>(state.invalidTileCoords),
            static_cast<unsigned long long>(state.tileOverflow));
        Emit(line);
        for(std::size_t i=0;i<std::min(n,HoverDiffPrintTiles);++i) {
            const auto& t=sorted[i];
            const double h=hoverMs?(323000.0/35.0)*double(t.hover)/double(hoverMs):0.0;
            const double a=awayMs?(323000.0/35.0)*double(t.away)/double(awayMs):0.0;
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_DIFF_TILE rank=%zu binX=%d binY=%d sampleXY=%.1f,%.1f hoverSampled=%u awaySampled=%u hoverEstimatedHz=%.2f awayEstimatedHz=%.2f excessHz=%.2f itemOwnership=unproven",
                i+1,int(t.binX),int(t.binY),double(t.sampleX),double(t.sampleY),
                unsigned(t.hover),unsigned(t.away),h,a,h-a);
            Emit(line);
        }
    }
    if (&state==&HoverDiffGlyph) {
        // The 0.1.71 capture has a horizontal set of 5 hover-only bins at
        // native y~726. Find such runs from data rather than hardcoding their
        // coordinates; 32-unit bins may have gaps between letters/words.
        std::array<HoverDiffTile,HoverDiffMaxTiles> exclusive{};
        std::size_t count{};
        for (std::size_t i=0;i<state.tileCount;++i) {
            const auto& t=state.tiles[i];
            if (t.hover>=20U && t.away==0U)exclusive[count++]=t;
        }
        std::sort(exclusive.begin(),exclusive.begin()+count,
            [](const auto& a,const auto& b) noexcept {
                return a.binY==b.binY ? a.binX<b.binX : a.binY<b.binY;
            });
        std::size_t bestBegin{},bestEnd{},bestHits{};
        for (std::size_t i=0;i<count;) {
            std::size_t j=i+1;
            std::size_t hits=exclusive[i].hover;
            while (j<count && exclusive[j].binY==exclusive[j-1].binY &&
                exclusive[j].binX-exclusive[j-1].binX<=3 &&
                exclusive[j].binX-exclusive[i].binX<=16) {
                hits+=exclusive[j].hover;
                ++j;
            }
            if (j-i>=3U && hits>bestHits) {
                bestBegin=i;bestEnd=j;bestHits=hits;
            }
            i=j;
        }
        const auto span=bestEnd-bestBegin;
        const bool clean=span>=3U && span<=16U &&
            HoverDiffOtherItemCallbacks.load(std::memory_order_acquire)==0U &&
            state.tileOverflow==0U && state.invalidTileCoords==0U &&
            HoverDiffItemId.load(std::memory_order_relaxed)!=0U;
        bool nativeWhite=clean;
        for (std::size_t i=bestBegin;i<bestEnd && nativeWhite;++i) {
            const auto& t=exclusive[i];
            if (!t.hoverRgbaValid || t.hoverRgba[3]<0.99f ||
                t.hoverRgba[3]>1.001f) {nativeWhite=false;break;}
            for (int c=0;c<3;++c) {
                if (!std::isfinite(t.hoverRgba[c]) ||
                    std::fabs(t.hoverRgba[c]-0.941f)>0.03f) {
                    nativeWhite=false;break;
                }
            }
        }
        const bool styleTrialAvailable =
            InWorldMode.load(std::memory_order_acquire) == InWorldBackend::SoEInterop &&
            InWorldStyleAttached.load(std::memory_order_acquire);
        if (!HoverRgbTrialEnabled) {
            Emit("LOOT_HOVER_RGB_POC_DISABLED version=1.0.0 reason=partial-name-tile-coverage full-native-label-ownership-unproven glyphColorWrites=0 backgroundWrites=0");
        } else if (nativeWhite && styleTrialAvailable &&
            HoverDiffCompleted.load(std::memory_order_acquire)) {
            HoverRgbTrialMinX.store(exclusive[bestBegin].binX,
                std::memory_order_relaxed);
            HoverRgbTrialMaxX.store(exclusive[bestEnd-1].binX,
                std::memory_order_relaxed);
            HoverRgbTrialRowY.store(exclusive[bestBegin].binY,
                std::memory_order_relaxed);
            HoverRgbTrialItemId.store(HoverDiffItemId.load(
                std::memory_order_acquire),std::memory_order_relaxed);
            HoverRgbTrialCode.store(HoverDiffCode.load(
                std::memory_order_acquire),std::memory_order_relaxed);
            HoverRgbTrialReady.store(true,std::memory_order_release);
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_RGB_POC_READY version=1.0.0 unitId=%u rowY=%d minX=%d maxX=%d nativeWhite=1 noNewHooks=1 trial=second-hover-only alpha=preserved bg=unmodified not-production-ownership-proof=1",
                HoverRgbTrialItemId.load(),
                exclusive[bestBegin].binY,exclusive[bestBegin].binX,
                exclusive[bestEnd-1].binX);
            Emit(line);
        } else {
            // The 0.1.73 capture produced one hover-exclusive tile (469
            // samples). Do not silently report this as an ambiguous color or
            // insufficient JSON rule: the training gate specifically requires
            // >=3 horizontal bins. Expose the failed gate and remaining
            // witnesses, keeping RGB writes disabled until ALL gates pass.
            const char* reason = !styleTrialAvailable ? "soe-style-interop-unavailable-standalone-readonly" :
                span<3U ? "insufficient-horizontal-coverage" :
                HoverDiffOtherItemCallbacks.load(std::memory_order_acquire)!=0U ? "other-matched-item-observed" :
                state.tileOverflow!=0U ? "tile-overflow" :
                state.invalidTileCoords!=0U ? "invalid-glyph-coordinates" :
                HoverDiffItemId.load(std::memory_order_relaxed)==0U ? "missing-runtime-item-id" :
                !nativeWhite ? "native-color-not-qualified" :
                "incomplete-training";
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_RGB_POC_REFUSED version=1.0.0 reason=%s exclusiveTiles=%zu qualifiedClusterTiles=%zu sampledHits=%zu candidateFirstTile=%d,%d nativeWhite=%u otherItemCallbacks=%llu overflow=%llu invalidCoords=%llu no-glyph-writes=1",
                reason,count,span,bestHits,
                count?exclusive[0].binX:0,count?exclusive[0].binY:0,
                nativeWhite?1U:0U,
                static_cast<unsigned long long>(HoverDiffOtherItemCallbacks.load(std::memory_order_acquire)),
                static_cast<unsigned long long>(state.tileOverflow),
                static_cast<unsigned long long>(state.invalidTileCoords));
            Emit(line);
        }
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_DIFF_EXCLUSIVE_CLUSTER version=1.0.0 candidate=%u qualifyingTiles=%zu rowY=%d firstX=%d lastX=%d sampledHits=%zu firstItemId=%u otherItemCallbacks=%llu rule=hover>=20,away=0,gap<=2-tiles,span<=16-tiles identity=temporal-spatial-candidate-only no-writes=1",
            span>=3U?1U:0U,count,
            span?int(exclusive[bestBegin].binY):0,
            span?int(exclusive[bestBegin].binX):0,
            span?int(exclusive[bestEnd-1].binX):0,bestHits,
            HoverDiffItemId.load(std::memory_order_acquire),
            static_cast<unsigned long long>(HoverDiffOtherItemCallbacks.load(
                std::memory_order_acquire)));
        Emit(line);
        for (std::size_t i=bestBegin;i<bestEnd;++i) {
            if (!span)break;
            const auto& t=exclusive[i];
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_DIFF_EXCLUSIVE_TILE version=1.0.0 binX=%d binY=%d x=%.1f y=%.1f hoverSamples=%u awaySamples=%u colorValid=%u nativeRgba=%.3f,%.3f,%.3f,%.3f provenance=unproven",
                int(t.binX),int(t.binY),double(t.sampleX),double(t.sampleY),
                unsigned(t.hover),unsigned(t.away),
                t.hoverRgbaValid?1U:0U,
                double(t.hoverRgba[0]),double(t.hoverRgba[1]),
                double(t.hoverRgba[2]),double(t.hoverRgba[3]));
            Emit(line);
        }
    }
    for (std::size_t phase=0;phase<2;++phase) {
        for (std::size_t i=0;i<state.stackCounts[phase];++i) {
            const auto& stack=state.stacks[phase][i];
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_DIFF_STACK renderer=%s phase=%s ordinal=%zu tid=%lu callerRva=D2R+0x%llX depth=%u",
                name,phase==0?"hover":"away",i,
                static_cast<unsigned long>(stack.thread),
                static_cast<unsigned long long>(stack.callerRva),
                unsigned(stack.count));
            Emit(line);
            for (std::uint16_t f=0;f<stack.count;++f) {
                const auto addr=stack.frames[f];
                const bool inImage=Base && addr>=Base && addr-Base<ImageSize;
                std::snprintf(line,sizeof(line),
                    "LOOT_HOVER_DIFF_STACK_FRAME renderer=%s phase=%s ordinal=%zu index=%u module=%s returnRva=0x%llX",
                    name,phase==0?"hover":"away",i,unsigned(f),
                    inImage?"D2R":"other",static_cast<unsigned long long>(
                        inImage?addr-Base:0));
                Emit(line);
            }
        }
    }
}

void ReportHoverNativeConsumerCode() noexcept {
    // Static bytes, not an assertion that the relay's caller draws text.
    // Capture the qualified merge-relay CALL and immediate continuation.
    constexpr std::uintptr_t begin=0xC0F67-0x30;
    constexpr std::size_t bytes=0x250;
    std::array<std::uint8_t,bytes> snapshot{};
    if (!ReadSafe(begin,snapshot.data(),snapshot.size())) {
        Emit("LOOT_HOVER_DIFF_NATIVE_WINDOW unavailable reason=read-failed");
        return;
    }
    Emit("LOOT_HOVER_DIFF_NATIVE_WINDOW_BEGIN build=93847 startRva=D2R+0xC0F37 callRva=D2R+0xC0F67 returnRva=D2R+0xC0F6C bytes=592 type=static-code-not-runtime-text-consumer-proof");
    char line[270]{};
    for (std::size_t at=0;at<bytes;at+=16) {
        char hex[16*3+1]{};
        for (std::size_t i=0;i<16;++i)
            std::snprintf(hex+i*3,sizeof(hex)-i*3,"%02X ",
                unsigned(snapshot[at+i]));
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_DIFF_NATIVE_BYTES rva=D2R+0x%llX hex='%s'",
            static_cast<unsigned long long>(begin+at),hex);
        Emit(line);
    }
    Emit("LOOT_HOVER_DIFF_NATIVE_WINDOW_END next=inspect-text-consumer-0xBFA00-and-later-continuation no-hooks-added=1");
    // Live bytes establish the actual target of the post-relay call seen in
    // this log, not its semantics. Do not hook/call this candidate until ABI
    // and its callers are established in the user's exact runtime image.
    constexpr std::uintptr_t consumerRva=0xBFA00;
    constexpr std::size_t consumerBytes=0x100;
    std::array<std::uint8_t,consumerBytes> consumer{};
    if (!ReadSafe(consumerRva,consumer.data(),consumer.size())) {
        Emit("LOOT_HOVER_DIFF_CONSUMER_WINDOW unavailable target=D2R+0xBFA00 reason=read-failed no-hooks-added=1");
        return;
    }
    Emit("LOOT_HOVER_DIFF_CONSUMER_WINDOW_BEGIN version=1.0.0 target=D2R+0xBFA00 sourceCall=D2R+0xC0F7C role=unverified entryBytes=256 no-hooks-added=1");
    for (std::size_t at=0;at<consumerBytes;at+=16) {
        char hex[16*3+1]{};
        for (std::size_t i=0;i<16;++i)
            std::snprintf(hex+i*3,sizeof(hex)-i*3,"%02X ",
                unsigned(consumer[at+i]));
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_DIFF_CONSUMER_BYTES rva=D2R+0x%llX hex='%s'",
            static_cast<unsigned long long>(consumerRva+at),hex);
        Emit(line);
    }
    Emit("LOOT_HOVER_DIFF_CONSUMER_WINDOW_END label-render-ownership=not-proven");
}

void ReportHoverRendererCandidateCode() noexcept {
    if (!Context || !Base) return;
    // Static code windows only. 0x908580 is the observed shared glyph-B return,
    // while 0x843C00 receives the post-relay label record at D2R+0xC1097.
    // Neither address is promoted to an item-specific rendering hook here.
    for (const auto& entry : std::array<std::pair<std::uintptr_t,
            std::size_t>,2>{{{0x908480,512},{0x843C00,256}}}) {
        const auto rva=entry.first;
        const auto size=entry.second;
        std::array<std::uint8_t,512> bytes{};
        if (!ReadSafe(rva,bytes.data(),size)) {
            char msg[200]{};
            std::snprintf(msg,sizeof(msg),
                "LOOT_HOVER_DIFF_TRACE_UNAVAILABLE startRva=D2R+0x%llX read-failed no-hooks=1",
                static_cast<unsigned long long>(rva));
            Emit(msg);
            continue;
        }
        char msg[400]{};
        std::snprintf(msg,sizeof(msg),
            "LOOT_HOVER_DIFF_TRACE_BEGIN version=1.0.0 startRva=D2R+0x%llX length=%zu type=static-code no-hook-install=1",
            static_cast<unsigned long long>(rva),size);
        Emit(msg);
        for (std::size_t at=0;at<size;at+=16) {
            char hex[52]{};
            for (std::size_t i=0;i<16;++i)
                std::snprintf(hex+i*3,sizeof(hex)-i*3,"%02X ",
                    unsigned(bytes[at+i]));
            std::snprintf(msg,sizeof(msg),
                "LOOT_HOVER_DIFF_TRACE_BYTES rva=D2R+0x%llX hex='%s'",
                static_cast<unsigned long long>(rva+at),hex);
            Emit(msg);
        }
    }
    Emit("LOOT_HOVER_DIFF_TRACE_END semantics=unverified disassemble-before-native-hooks=1");
}

// 0.1.99: recovery and native hidden-hover callsite provenance.
// Previous builds installed an inline detour at D2R+0x657B90, which is shared
// by many native rectangles. The user observed missing inventory and ground
// tooltip fills even when JSON recoloring refused to activate. There is NO
// hook, function-pointer call, instruction patch, geometry write or RGB write
// for either the shared function or its hidden-hover caller in this build.
// This probes only code bytes from the running build for OFFLINE disassembly.
void ReportNativeHoverBackgroundCallsite() noexcept {
    constexpr std::uintptr_t callRva=0x8DA9F3;
    constexpr std::uintptr_t returnRva=0x8DA9F8;
    constexpr std::uintptr_t observedTargetRva=0x657B90;
    const char* version=Context?D2RL::GetBuildName(Context):nullptr;
    if (!version || std::string_view(version)!="93847" || !Base || !ImageSize) {
        Emit("LOOT_HOVER_BG_CALLSITE_UNAVAILABLE version=1.0.0 reason=build-or-image-unavailable unsafeRectHook=absent no-fallback=1");
        return;
    }
    std::array<std::uint8_t,5> call{};
    std::array<std::uint8_t,19> entry{};
    if (!ReadSafe(callRva,call.data(),call.size()) ||
        !ReadSafe(observedTargetRva,entry.data(),entry.size())) {
        Emit("LOOT_HOVER_BG_CALLSITE_UNAVAILABLE version=1.0.0 reason=read-failed unsafeRectHook=absent no-fallback=1");
        return;
    }
    std::int32_t relative{};
    std::memcpy(&relative,call.data()+1,sizeof(relative));
    const auto target=static_cast<std::int64_t>(returnRva)+relative;
    constexpr std::array<std::uint8_t,19> knownEntry{{
        0x44,0x3B,0xC1,0x0F,0x8C,0xD3,0x02,0x00,0x00,
        0x4C,0x8B,0xDC,0x55,0x41,0x54,0x41,0x55,0x41,0x57
    }};
    const bool callValid=call[0]==0xE8 &&
        target==static_cast<std::int64_t>(observedTargetRva);
    const bool entryValid=entry==knownEntry;
    // Unwind metadata describes the containing function, not the renderer's
    // arguments or whether a particular draw belongs to a given item.
    DWORD64 imageBase{};
    const auto* owner=RtlLookupFunctionEntry(
        static_cast<DWORD64>(Base+callRva),&imageBase,nullptr);
    char line[320]{};
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_CALLSITE_READY version=1.0.0 callRva=D2R+0x%llX returnRva=D2R+0x%llX decodedTarget=D2R+0x%llX callMatchesHistorical=%u sharedEntryMatchesHistorical=%u unwind=%u ownerBeginRva=D2R+0x%llX ownerEndRva=D2R+0x%llX unsafeRectHook=absent codeWrites=0 tintWrites=0",
        static_cast<unsigned long long>(callRva),
        static_cast<unsigned long long>(returnRva),
        static_cast<unsigned long long>(target>=0?target:0),
        callValid?1U:0U,entryValid?1U:0U,owner?1U:0U,
        owner?static_cast<unsigned long long>(owner->BeginAddress):0ULL,
        owner?static_cast<unsigned long long>(owner->EndAddress):0ULL);
    Emit(line);
    // A failed fingerprint must not trigger a fallback hook, recoloring or
    // assumptions about what the native call actually does.
    if (!callValid || !entryValid) {
        Emit("LOOT_HOVER_BG_CALLSITE_STOP version=1.0.0 reason=call-or-target-fingerprint-mismatch unsafeRectHook=absent no-fallback=1");
        return;
    }
    // Deliberately bounded near CALL: 256 bytes before and 256 after, for
    // the actual argument preparation and post-call continuation. The start
    // is not assumed to be an x86 instruction boundary.
    constexpr std::uintptr_t start=callRva-0x100;
    constexpr std::size_t length=0x200;
    std::array<std::uint8_t,length> snapshot{};
    if (!ReadSafe(start,snapshot.data(),snapshot.size())) {
        Emit("LOOT_HOVER_BG_CALLSITE_UNAVAILABLE version=1.0.0 reason=call-neighborhood-read-failed unsafeRectHook=absent");
        return;
    }
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_CALLSITE_WINDOW_BEGIN version=1.0.0 startRva=D2R+0x%llX length=%zu callRva=D2R+0x%llX type=static-code-not-instruction-aligned unsafeRectHook=absent",
        static_cast<unsigned long long>(start),length,
        static_cast<unsigned long long>(callRva));
    Emit(line);
    for (std::size_t at=0;at<length;at+=16) {
        char hex[16*3+1]{};
        for (std::size_t i=0;i<16;++i)
            std::snprintf(hex+i*3,sizeof(hex)-i*3,"%02X ",
                unsigned(snapshot[at+i]));
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_BG_CALLSITE_BYTES rva=D2R+0x%llX hex='%s'",
            static_cast<unsigned long long>(start+at),hex);
        Emit(line);
    }
    Emit("LOOT_HOVER_BG_CALLSITE_WINDOW_END version=1.0.0 next=offline-disassemble-owner-and-ABI-before-any-new-hooks nativeBackground=unchanged overlay=0");
}

// 0.1.99: validate the origin of the color pointer passed by the two
// native hidden-hover rectangle CALLs. The 0.1.78 code window reveals
//     lea r14,[rsi+0x168]
//     mov [rsp+0x20],r14 ; call 0x657B90 (first rectangle)
//     mov [rsp+0x20],r14 ; call 0x657B90 (second rectangle)
// The +0x168 address is a member within an UNVERIFIED native object, not a
// proved universal "background-color offset". Only static byte reads here;
// no hooks, pointer dereferences, gameplay item edits, or color substitution.
void ReportNativeHoverBackgroundR14Provenance() noexcept {
    constexpr std::uintptr_t firstCall=0x8DA9B4;
    constexpr std::uintptr_t secondCall=0x8DA9F3;
    constexpr std::uintptr_t targetRva=0x657B90;
    constexpr std::uintptr_t leaRva=0x8DA91B;
    constexpr std::uintptr_t firstArgumentRva=0x8DA952;
    constexpr std::uintptr_t secondArgumentRva=0x8DA9EE;
    constexpr std::array<std::uint8_t,7> expectedLea{{
        0x4C,0x8D,0xB6,0x68,0x01,0x00,0x00
    }};
    constexpr std::array<std::uint8_t,5> expectedArg{{
        0x4C,0x89,0x74,0x24,0x20
    }};
    constexpr std::uintptr_t ownerWindowBegin=0x8DA5F3;
    constexpr std::size_t ownerWindowLength=0x300;
    const char* build=Context?D2RL::GetBuildName(Context):nullptr;
    if (!build || std::string_view(build)!="93847" || !Base || !ImageSize) {
        Emit("LOOT_HOVER_BG_R14_UNAVAILABLE version=1.0.0 reason=build-or-image-unavailable no-hooks=1 no-writes=1");
        return;
    }
    constexpr std::array<std::uint8_t,19> expectedSharedEntry{{
        0x44,0x3B,0xC1,0x0F,0x8C,0xD3,0x02,0x00,0x00,
        0x4C,0x8B,0xDC,0x55,0x41,0x54,0x41,0x55,0x41,0x57
    }};
    std::array<std::uint8_t,19> sharedEntry{};
    std::array<std::uint8_t,7> lea{};
    std::array<std::uint8_t,5> firstArg{},secondArg{},call1{},call2{};
    if (!ReadSafe(targetRva,sharedEntry.data(),sharedEntry.size()) ||
        !ReadSafe(leaRva,lea.data(),lea.size()) ||
        !ReadSafe(firstArgumentRva,firstArg.data(),firstArg.size()) ||
        !ReadSafe(secondArgumentRva,secondArg.data(),secondArg.size()) ||
        !ReadSafe(firstCall,call1.data(),call1.size()) ||
        !ReadSafe(secondCall,call2.data(),call2.size())) {
        Emit("LOOT_HOVER_BG_R14_UNAVAILABLE version=1.0.0 reason=static-code-read-failed no-hooks=1 no-writes=1");
        return;
    }
    const auto callTarget=[](const std::array<std::uint8_t,5>& bytes,
                            std::uintptr_t returnRva) noexcept -> std::int64_t {
        std::int32_t displacement{};
        std::memcpy(&displacement,bytes.data()+1,sizeof(displacement));
        return static_cast<std::int64_t>(returnRva)+displacement;
    };
    const bool firstCallOk=call1[0]==0xE8 &&
        callTarget(call1,firstCall+5)==static_cast<std::int64_t>(targetRva);
    const bool secondCallOk=call2[0]==0xE8 &&
        callTarget(call2,secondCall+5)==static_cast<std::int64_t>(targetRva);
    const bool sharedEntryOk=sharedEntry==expectedSharedEntry;
    const bool leaOk=lea==expectedLea;
    const bool firstArgOk=firstArg==expectedArg;
    const bool secondArgOk=secondArg==expectedArg;
    char line[520]{};
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_R14_READY version=1.0.0 verified=%u sharedEntryMatches=%u leaRva=D2R+0x%llX leaMatches=%u firstArgRva=D2R+0x%llX firstArgMatches=%u firstCallRva=D2R+0x%llX firstCallMatches=%u secondArgRva=D2R+0x%llX secondArgMatches=%u secondCallRva=D2R+0x%llX secondCallMatches=%u target=D2R+0x657B90 no-hooks=1 no-writes=1",
        (sharedEntryOk&&leaOk&&firstArgOk&&secondArgOk&&firstCallOk&&secondCallOk)?1U:0U,
        sharedEntryOk?1U:0U,
        static_cast<unsigned long long>(leaRva),leaOk?1U:0U,
        static_cast<unsigned long long>(firstArgumentRva),firstArgOk?1U:0U,
        static_cast<unsigned long long>(firstCall),firstCallOk?1U:0U,
        static_cast<unsigned long long>(secondArgumentRva),secondArgOk?1U:0U,
        static_cast<unsigned long long>(secondCall),secondCallOk?1U:0U);
    Emit(line);
    if (!sharedEntryOk || !leaOk || !firstArgOk || !secondArgOk || !firstCallOk || !secondCallOk) {
        Emit("LOOT_HOVER_BG_R14_STOP version=1.0.0 reason=instruction-or-call-fingerprint-mismatch no-assumed-field-layout=1 no-fallback-hooks=1");
        return;
    }
    Emit("LOOT_HOVER_BG_R14_SOURCE version=1.0.0 expression=address-of-[RSI+0x168] encoded=LEA-R14-[RSI+0x168] stackFifthArg=R14 sharedByBothCalls=1 nativeObjectType=unverified fieldMeaning=unverified runtimePointerRead=0 nativeColorWrites=0");
    // Previous 0.1.78 snapshot starts at 0x8DA8F3, which is *after* the
    // instruction assigning RSI may occur. Capture the preceding 0x300 bytes
    // to identify its owner/provenance. This start isn't assumed to be an
    // instruction boundary, nor is the whole window claimed to be one fn.
    std::array<std::uint8_t,ownerWindowLength> snapshot{};
    if (!ReadSafe(ownerWindowBegin,snapshot.data(),snapshot.size())) {
        Emit("LOOT_HOVER_BG_OWNER_UNAVAILABLE version=1.0.0 reason=prior-code-window-read-failed no-hooks=1");
        return;
    }
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_OWNER_WINDOW_BEGIN version=1.0.0 startRva=D2R+0x%llX length=%zu endsBefore=D2R+0x8DA8F3 instructionAligned=unverified readOnly=1",
        static_cast<unsigned long long>(ownerWindowBegin),ownerWindowLength);
    Emit(line);
    for (std::size_t at=0;at<snapshot.size();at+=16) {
        char hex[16*3+1]{};
        for (std::size_t i=0;i<16;++i)
            std::snprintf(hex+i*3,sizeof(hex)-i*3,"%02X ",unsigned(snapshot[at+i]));
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_BG_OWNER_BYTES rva=D2R+0x%llX hex='%s'",
            static_cast<unsigned long long>(ownerWindowBegin+at),hex);
        Emit(line);
    }
    Emit("LOOT_HOVER_BG_OWNER_WINDOW_END version=1.0.0 next=identify-RSI-object-origin-and-verified-native-color-field no-hook-or-writes=1");
}

// 0.1.99: locate DIRECT CALL references to the already verified native UI
// paint-object renderer. This reads ONLY D2R's executable .text section,
// once on the diagnostic reporter thread, after the first matched item hover.
// A five-byte E8 displacement can appear in data or inside an instruction;
// the results are candidates for offline disassembly, NOT hook-qualified RVAs.
// No call interception, renderer invocation or object-memory dereference.
void ReportNativeHoverBackgroundOwnerCallers() noexcept {
    constexpr std::uintptr_t renderEntryRva=0x8DA7E0;
    constexpr std::uintptr_t ownerMovRva=0x8DA802;
    constexpr std::uintptr_t colorLeaRva=0x8DA91B;
    constexpr std::array<std::uint8_t,16> expectedEntry{{
        0x40,0x56,0x48,0x81,0xEC,0x10,0x01,0x00,
        0x00,0x48,0x8B,0x05,0xD8,0x0A,0x0F,0x02
    }};
    constexpr std::array<std::uint8_t,3> expectedMov{{0x48,0x8B,0xF1}};
    constexpr std::array<std::uint8_t,7> expectedLea{{
        0x4C,0x8D,0xB6,0x68,0x01,0x00,0x00
    }};
    const char* build=Context?D2RL::GetBuildName(Context):nullptr;
    if (!build || std::string_view(build)!="93847" || !Base || !ImageSize) {
        Emit("LOOT_HOVER_BG_OWNER_XREF_UNAVAILABLE version=1.0.0 reason=build-or-image no-hooks=1");
        return;
    }
    std::array<std::uint8_t,16> entry{};
    std::array<std::uint8_t,3> mov{};
    std::array<std::uint8_t,7> lea{};
    if (!ReadSafe(renderEntryRva,entry.data(),entry.size()) ||
        !ReadSafe(ownerMovRva,mov.data(),mov.size()) ||
        !ReadSafe(colorLeaRva,lea.data(),lea.size()) ||
        entry!=expectedEntry || mov!=expectedMov || lea!=expectedLea) {
        Emit("LOOT_HOVER_BG_OWNER_XREF_REFUSED version=1.0.0 reason=owner-entry-or-RCX-to-RSI-or-color-LEA-fingerprint-mismatch no-hooks=1");
        return;
    }
    char line[512]{};
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_OWNER_PROVENANCE version=1.0.0 entry=D2R+0x%llX input=RCX movRsiRva=D2R+0x%llX expression=address-of-[RCX+0x168] rgbaPassedBy=R14-to-fifth-stack-arg role=renderer-object-not-gameplay-unit nativeColorWrites=0",
        static_cast<unsigned long long>(renderEntryRva),
        static_cast<unsigned long long>(ownerMovRva));
    Emit(line);
    IMAGE_DOS_HEADER dos{};
    if (!ReadSafe(0,&dos,sizeof(dos)) ||
        dos.e_magic!=IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew<=0 || dos.e_lfanew>0x1000) {
        Emit("LOOT_HOVER_BG_OWNER_XREF_UNAVAILABLE version=1.0.0 reason=dos-header no-hooks=1");
        return;
    }
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadSafe(static_cast<std::uintptr_t>(dos.e_lfanew),&nt,sizeof(nt)) ||
        nt.Signature!=IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections==0 ||
        nt.FileHeader.NumberOfSections>96) {
        Emit("LOOT_HOVER_BG_OWNER_XREF_UNAVAILABLE version=1.0.0 reason=nt-header-or-section-count no-hooks=1");
        return;
    }
    const auto sectionRva=static_cast<std::uintptr_t>(dos.e_lfanew)+
        sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+
        nt.FileHeader.SizeOfOptionalHeader;
    std::array<IMAGE_SECTION_HEADER,96> sections{};
    const auto sectionBytes=std::size_t(nt.FileHeader.NumberOfSections)*
        sizeof(IMAGE_SECTION_HEADER);
    if (!ReadSafe(sectionRva,sections.data(),sectionBytes)) {
        Emit("LOOT_HOVER_BG_OWNER_XREF_UNAVAILABLE version=1.0.0 reason=section-header-read no-hooks=1");
        return;
    }
    std::uintptr_t begin{};
    std::size_t textSize{};
    for (std::size_t i=0;i<nt.FileHeader.NumberOfSections;++i) {
        const auto& part=sections[i];
        if (std::memcmp(part.Name,".text",5)!=0 ||
            !(part.Characteristics&IMAGE_SCN_MEM_EXECUTE)) continue;
        begin=part.VirtualAddress;
        textSize=part.Misc.VirtualSize;
        break;
    }
    if (!begin || begin>=ImageSize || !textSize ||
        textSize>0x8000000U || textSize>ImageSize-begin) {
        Emit("LOOT_HOVER_BG_OWNER_XREF_UNAVAILABLE version=1.0.0 reason=text-section-bounds no-hooks=1");
        return;
    }
    constexpr std::size_t chunkSize=0x4000;
    std::vector<std::uint8_t> bytes{};
    try {bytes.resize(chunkSize+4);} catch (const std::exception&) {
        Emit("LOOT_HOVER_BG_OWNER_XREF_UNAVAILABLE version=1.0.0 reason=scan-buffer-allocation no-hooks=1");
        return;
    }
    std::uint64_t candidates{};
    std::uint64_t failedChunks{};
    std::uint64_t windows{};
    constexpr std::size_t maxPrinted=24;
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_OWNER_XREF_BEGIN version=1.0.0 scan=.text begin=D2R+0x%llX bytes=%zu target=D2R+0x%llX E8-rel32-candidates-only=1 readOnly=1 newHooks=0",
        static_cast<unsigned long long>(begin),textSize,
        static_cast<unsigned long long>(renderEntryRva));
    Emit(line);
    const auto end=begin+textSize;
    for (auto at=begin;at<end;at+=chunkSize) {
        const auto actual=std::min<std::size_t>(chunkSize+4,end-at);
        if (actual<5 || !ReadSafe(at,bytes.data(),actual)) {
            ++failedChunks;
            continue;
        }
        const auto scanCount=std::min<std::size_t>(chunkSize,actual-4);
        for (std::size_t i=0;i<scanCount;++i) {
            if (bytes[i]!=0xE8) continue;
            std::int32_t displacement{};
            std::memcpy(&displacement,bytes.data()+i+1,sizeof(displacement));
            const auto destination=static_cast<std::int64_t>(at+i+5)+
                static_cast<std::int64_t>(displacement);
            if (destination!=static_cast<std::int64_t>(renderEntryRva))continue;
            ++candidates;
            if (windows>=maxPrinted)continue;
            const auto callRva=at+i;
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_BG_OWNER_XREF_SITE version=1.0.0 callRva=D2R+0x%llX returnRva=D2R+0x%llX target=D2R+0x%llX interpretation=potential-direct-call-not-instruction-aligned",
                static_cast<unsigned long long>(callRva),
                static_cast<unsigned long long>(callRva+5),
                static_cast<unsigned long long>(renderEntryRva));
            Emit(line);
            constexpr std::size_t pre=32;
            constexpr std::size_t post=64;
            if (callRva>=pre && callRva+post<=ImageSize) {
                std::array<std::uint8_t,pre+post> nearBytes{};
                if (ReadSafe(callRva-pre,nearBytes.data(),nearBytes.size())) {
                    for (std::size_t j=0;j<nearBytes.size();j+=16) {
                        char hex[49]{};
                        for (std::size_t k=0;k<16;++k)
                            std::snprintf(hex+k*3,sizeof(hex)-k*3,
                                "%02X ",unsigned(nearBytes[j+k]));
                        std::snprintf(line,sizeof(line),
                            "LOOT_HOVER_BG_OWNER_XREF_BYTES callRva=D2R+0x%llX rva=D2R+0x%llX hex='%s'",
                            static_cast<unsigned long long>(callRva),
                            static_cast<unsigned long long>(callRva-pre+j),hex);
                        Emit(line);
                    }
                }
            }
            ++windows;
        }
    }
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_OWNER_XREF_END version=1.0.0 candidateCalls=%llu printed=%llu failedChunks=%llu scanLimitedToD2RText=1 nativeHooksAdded=0 nativeItemWrites=0 backgroundWrites=0 no-overlay=1 next=offline-disassemble-caller-and-ownership",
        static_cast<unsigned long long>(candidates),
        static_cast<unsigned long long>(windows),
        static_cast<unsigned long long>(failedChunks));
    Emit(line);
}

// 0.1.99: follow the single direct call candidate found by the 0.1.80
// build-93847 .text scan. The callsite's preceding two instructions show a
// stride-based address calculation: RCX = *(RDI) + *(RSP+0x40) * 0x2E8.
// This is code provenance only. The owner of RDI, the index, the lifetime of
// the selected render object, and whether it represents an item remain
// unverified. No native hook, runtime object read, or color write is added.
void ReportNativeHoverBackgroundOwnerArray() noexcept {
    constexpr std::uintptr_t callRva=0x880BC7;
    constexpr std::uintptr_t targetRva=0x8DA7E0;
    constexpr std::uintptr_t addressRva=0x880BBB;
    constexpr std::array<std::uint8_t,17> expected{{
        0x48,0x69,0x4C,0x24,0x40,0xE8,0x02,0x00,0x00,
        0x48,0x03,0x0F,
        0xE8,0x14,0x9C,0x05,0x00
    }};
    const char* build=Context?D2RL::GetBuildName(Context):nullptr;
    if (!build || std::string_view(build)!="93847" || !Base || !ImageSize) {
        Emit("LOOT_HOVER_BG_OWNER_ARRAY_UNAVAILABLE version=1.0.0 reason=build-or-image no-hooks=1 no-writes=1");
        return;
    }
    std::array<std::uint8_t,expected.size()> instructions{};
    if (!ReadSafe(addressRva,instructions.data(),instructions.size())) {
        Emit("LOOT_HOVER_BG_OWNER_ARRAY_UNAVAILABLE version=1.0.0 reason=static-code-read-failed no-hooks=1 no-writes=1");
        return;
    }
    std::int32_t displacement{};
    std::memcpy(&displacement,instructions.data()+13,sizeof(displacement));
    const auto decoded=static_cast<std::int64_t>(callRva+5)+displacement;
    const bool qualified=instructions==expected &&
        decoded==static_cast<std::int64_t>(targetRva);
    char line[360]{};
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_OWNER_ARRAY_READY version=1.0.0 verified=%u caller=D2R+0x%llX target=D2R+0x%llX index=stack-[RSP+0x40] stride=0x2E8 base=pointer-[RDI] colorOffsetWithinRenderObject=0x168 nativeObjectLifetime=unverified itemIdentity=unverified no-hooks=1 no-writes=1",
        qualified?1U:0U,static_cast<unsigned long long>(callRva),
        static_cast<unsigned long long>(decoded>=0?decoded:0));
    Emit(line);
    if (!qualified) {
        Emit("LOOT_HOVER_BG_OWNER_ARRAY_STOP version=1.0.0 reason=caller-instruction-fingerprint-mismatch no-fallback-hook=1");
        return;
    }
    // Unwind lookup is diagnostic only; some build-93847 native functions
    // legitimately have no returned unwind entry. Never require it to read
    // code or infer function type from the lack of an entry.
    DWORD64 imageBase{};
    const auto* owner=RtlLookupFunctionEntry(
        static_cast<DWORD64>(Base+callRva),&imageBase,nullptr);
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_OWNER_ARRAY_UNWIND version=1.0.0 available=%u beginRva=D2R+0x%llX endRva=D2R+0x%llX objectType=unverified",
        owner?1U:0U,
        owner?static_cast<unsigned long long>(owner->BeginAddress):0ULL,
        owner?static_cast<unsigned long long>(owner->EndAddress):0ULL);
    Emit(line);
    // Read the continuous native call-owner neighborhood once on the
    // reporter thread, not the UI render thread. The arbitrary start RVA
    // is NOT claimed to be an instruction or function boundary.
    constexpr std::uintptr_t begin=0x880160;
    constexpr std::size_t count=0xB00;
    std::array<std::uint8_t,count> bytes{};
    if (!ReadSafe(begin,bytes.data(),bytes.size())) {
        Emit("LOOT_HOVER_BG_OWNER_ARRAY_UNAVAILABLE version=1.0.0 reason=owner-code-window-read-failed no-hooks=1");
        return;
    }
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_OWNER_ARRAY_WINDOW_BEGIN version=1.0.0 startRva=D2R+0x%llX bytes=%zu callRva=D2R+0x%llX instructionAligned=unverified nativeObjectReads=0 no-hooks=1",
        static_cast<unsigned long long>(begin),count,
        static_cast<unsigned long long>(callRva));
    Emit(line);
    for (std::size_t at=0;at<bytes.size();at+=32) {
        char hex[32*3+1]{};
        for (std::size_t i=0;i<32;++i)
            std::snprintf(hex+i*3,sizeof(hex)-i*3,"%02X ",
                unsigned(bytes[at+i]));
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_BG_OWNER_ARRAY_BYTES rva=D2R+0x%llX hex='%s'",
            static_cast<unsigned long long>(begin+at),hex);
        Emit(line);
    }
    Emit("LOOT_HOVER_BG_OWNER_ARRAY_WINDOW_END version=1.0.0 target=trace-RDI-source-and-native-record-construction before-any-owner-scoped-hook=1 global-rect-detour=0 backgroundWrites=0 overlay=0");
}


// 0.1.99: static provenance of the element APPEND/construction path.
// Build-93847 code establishes that the draw loop uses the container at
// parent+0x168 and that a separate function at 0x880160 also uses an
// RCX+0x168 container and calls the element constructor at 0x8D9EB0.
// These could be the same native UI array, but this is NOT item ownership
// proof. Inspect the direct callers before hooking or writing the array.
// Read executable bytes only on the automatic reporter thread. Never detour
// the shared native fill-rectangle function or its owner draw loop.
void ReportNativeHoverBackgroundProducerCallers() noexcept {
    constexpr std::uintptr_t insertEntry=0x880160;
    constexpr std::uintptr_t constructEntry=0x8D9EB0;
    constexpr std::uintptr_t appendArrayLea=0x88017A;
    constexpr std::uintptr_t renderArrayLea=0x880B4A;
    constexpr std::uintptr_t renderArrayCount=0x880B14;
    constexpr std::uintptr_t constructorCall=0x8801DD;
    constexpr std::array<std::uint8_t,7> appendFingerprint{{0x4C,0x8D,0xB1,0x68,0x01,0x00,0x00}};
    constexpr std::array<std::uint8_t,7> renderFingerprint{{0x49,0x8D,0xBF,0x68,0x01,0x00,0x00}};
    constexpr std::array<std::uint8_t,7> countFingerprint{{0x4D,0x39,0xB7,0x70,0x01,0x00,0x00}};
    constexpr std::array<std::uint8_t,5> constructFingerprint{{0xE8,0xCE,0x9C,0x05,0x00}};
    const char* build=Context?D2RL::GetBuildName(Context):nullptr;
    if (!build || std::string_view(build)!="93847" || !Base || !ImageSize) {
        Emit("LOOT_HOVER_BG_PRODUCER_UNAVAILABLE version=1.0.0 reason=build-or-image no-hooks=1");
        return;
    }
    std::array<std::uint8_t,7> append{},render{},count{};
    std::array<std::uint8_t,5> construct{};
    if (!ReadSafe(appendArrayLea,append.data(),append.size()) ||
        !ReadSafe(renderArrayLea,render.data(),render.size()) ||
        !ReadSafe(renderArrayCount,count.data(),count.size()) ||
        !ReadSafe(constructorCall,construct.data(),construct.size()) ||
        append!=appendFingerprint || render!=renderFingerprint ||
        count!=countFingerprint || construct!=constructFingerprint) {
        Emit("LOOT_HOVER_BG_PRODUCER_UNAVAILABLE version=1.0.0 reason=shared-native-container-fingerprint no-hooks=1 no-writes=1");
        return;
    }
    char line[460]{};
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_PRODUCER_READY version=1.0.0 insertEntry=D2R+0x%llX appendContainer=RCX+0x168 drawContainer=R15+0x168 drawCount=R15+0x170 elementStride=0x2E8 rgbaOffset=element+0x168 constructor=D2R+0x%llX sameContainerAtRuntime=unverified itemIdentity=unverified noHooks=1 noWrites=1",
        static_cast<unsigned long long>(insertEntry),
        static_cast<unsigned long long>(constructEntry));
    Emit(line);
    // Reuse already qualified PE section metadata from the preceding owner
    // scanner; the scan here is independent and bounded to executable .text.
    IMAGE_DOS_HEADER dos{};
    if (!ReadSafe(0,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew<0 || static_cast<std::size_t>(dos.e_lfanew)>0x100000) {
        Emit("LOOT_HOVER_BG_PRODUCER_UNAVAILABLE version=1.0.0 reason=pe-dos-header no-hooks=1");
        return;
    }
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadSafe(static_cast<std::uintptr_t>(dos.e_lfanew),&nt,sizeof(nt)) ||
        nt.Signature!=IMAGE_NT_SIGNATURE ||
        nt.FileHeader.NumberOfSections==0 ||
        nt.FileHeader.NumberOfSections>96) {
        Emit("LOOT_HOVER_BG_PRODUCER_UNAVAILABLE version=1.0.0 reason=pe-nt-header no-hooks=1");
        return;
    }
    const auto sectionRva=static_cast<std::uintptr_t>(dos.e_lfanew)+
        sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+
        nt.FileHeader.SizeOfOptionalHeader;
    std::array<IMAGE_SECTION_HEADER,96> sections{};
    if (!ReadSafe(sectionRva,sections.data(),
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER))) {
        Emit("LOOT_HOVER_BG_PRODUCER_UNAVAILABLE version=1.0.0 reason=section-header no-hooks=1");
        return;
    }
    std::uintptr_t begin{};
    std::size_t textSize{};
    for (std::size_t i=0;i<nt.FileHeader.NumberOfSections;++i) {
        const auto& sec=sections[i];
        if (std::memcmp(sec.Name,".text",5)!=0 ||
            !(sec.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        begin=sec.VirtualAddress;
        textSize=sec.Misc.VirtualSize;
        break;
    }
    if (!begin || begin>=ImageSize || !textSize ||
        textSize>0x8000000U || textSize>ImageSize-begin) {
        Emit("LOOT_HOVER_BG_PRODUCER_UNAVAILABLE version=1.0.0 reason=executable-text-bounds no-hooks=1");
        return;
    }
    constexpr std::size_t chunkSize=0x4000;
    std::vector<std::uint8_t> bytes{};
    try {bytes.resize(chunkSize+4);} catch (const std::exception&) {
        Emit("LOOT_HOVER_BG_PRODUCER_UNAVAILABLE version=1.0.0 reason=scan-buffer-allocation no-hooks=1");
        return;
    }
    // 0.1.99: also locate direct callers of the two functions that feed
    // native element data into the append/constructor chain. These are
    // candidate CALL byte sequences, not item ownership or render scope.
    constexpr std::uintptr_t labelAppendOwner=0x843CA0;
    constexpr std::uintptr_t otherConstructorOwner=0x87FF00;
    constexpr std::array<std::uint8_t,7> labelOwnerExpected{{
        0x48,0x89,0x6C,0x24,0x20,0x57,0x41}};
    constexpr std::array<std::uint8_t,7> otherOwnerExpected{{
        0x40,0x55,0x53,0x56,0x57,0x41,0x56}};
    std::array<std::uint8_t,7> labelOwnerBytes{},otherOwnerBytes{};
    const bool sourceOwnersVerified=ReadSafe(labelAppendOwner,
            labelOwnerBytes.data(),labelOwnerBytes.size()) &&
        ReadSafe(otherConstructorOwner,otherOwnerBytes.data(),
            otherOwnerBytes.size()) &&
        labelOwnerBytes==labelOwnerExpected &&
        otherOwnerBytes==otherOwnerExpected;
    Emit(sourceOwnersVerified?
        "LOOT_HOVER_BG_COLOR_SOURCE_READY version=1.0.0 labelOwner=D2R+0x843CA0 otherOwner=D2R+0x87FF00 staticEntryFingerprints=verified itemIdentity=unverified no-hooks=1 no-writes=1":
        "LOOT_HOVER_BG_COLOR_SOURCE_UNAVAILABLE version=1.0.0 reason=source-owner-entry-fingerprint no-source-scan=1 no-hooks=1 no-writes=1");
    constexpr std::array<std::uintptr_t,4> targets{{
        insertEntry,constructEntry,labelAppendOwner,otherConstructorOwner}};
    // One counter for EACH entry in targets. 0.1.85 mistakenly allocated
    // only two slots while subsequently indexing [2] and [3], which corrupted
    // adjacent stack storage and produced nonsensical caller totals.
    std::array<std::uint64_t,targets.size()> candidates{};
    std::array<std::uint64_t,targets.size()> printed{};
    static_assert(candidates.size()==targets.size());
    static_assert(printed.size()==targets.size());
    std::uint64_t failedChunks{};
    constexpr std::uint64_t maxPerTarget=16;
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_PRODUCER_XREF_BEGIN version=1.0.0 textBegin=D2R+0x%llX textBytes=%zu producer=D2R+0x%llX constructor=D2R+0x%llX E8-rel32-candidates-only=1 indirectCallersNotCovered=1 noHooks=1",
        static_cast<unsigned long long>(begin),textSize,
        static_cast<unsigned long long>(insertEntry),
        static_cast<unsigned long long>(constructEntry));
    Emit(line);
    const auto end=begin+textSize;
    for (auto at=begin;at<end;at+=chunkSize) {
        const auto actual=std::min<std::size_t>(chunkSize+4,end-at);
        if (actual<5 || !ReadSafe(at,bytes.data(),actual)) {
            ++failedChunks;
            continue;
        }
        const auto scanCount=std::min<std::size_t>(chunkSize,actual-4);
        for (std::size_t i=0;i<scanCount;++i) {
            if (bytes[i]!=0xE8)continue;
            std::int32_t displacement{};
            std::memcpy(&displacement,bytes.data()+i+1,sizeof(displacement));
            const auto destination=static_cast<std::int64_t>(at+i+5)+
                static_cast<std::int64_t>(displacement);
            for (std::size_t k=0;k<targets.size();++k) {
                if (k>=2 && !sourceOwnersVerified)continue;
                if (destination!=static_cast<std::int64_t>(targets[k]))continue;
                ++candidates[k];
                if (printed[k]>=maxPerTarget)break;
                const auto callRva=at+i;
                const char* kind=k==0?"append":k==1?"construct":
                    k==2?"label-owner":"other-constructor-owner";
                std::snprintf(line,sizeof(line),
                    "%s version=1.0.0 kind=%s callRva=D2R+0x%llX returnRva=D2R+0x%llX target=D2R+0x%llX aligned=unverified itemIdentity=unverified no-hooks=1",
                    k<2?"LOOT_HOVER_BG_PRODUCER_XREF_SITE":
                        "LOOT_HOVER_BG_COLOR_SOURCE_SITE",
                    kind,
                    static_cast<unsigned long long>(callRva),
                    static_cast<unsigned long long>(callRva+5),
                    static_cast<unsigned long long>(targets[k]));
                Emit(line);
                constexpr std::size_t pre=48,post=48;
                if (callRva>=pre && callRva+post<=ImageSize) {
                    std::array<std::uint8_t,pre+post> contextBytes{};
                    if (ReadSafe(callRva-pre,contextBytes.data(),contextBytes.size())) {
                        for (std::size_t j=0;j<contextBytes.size();j+=16) {
                            char hex[49]{};
                            for (std::size_t b=0;b<16;++b)
                                std::snprintf(hex+b*3,sizeof(hex)-b*3,
                                    "%02X ",unsigned(contextBytes[j+b]));
                            std::snprintf(line,sizeof(line),
                                "%s kind=%s callRva=D2R+0x%llX rva=D2R+0x%llX hex='%s'",
                                k<2?"LOOT_HOVER_BG_PRODUCER_XREF_BYTES":
                                    "LOOT_HOVER_BG_COLOR_SOURCE_BYTES",
                                kind,
                                static_cast<unsigned long long>(callRva),
                                static_cast<unsigned long long>(callRva-pre+j),hex);
                            Emit(line);
                        }
                    }
                }
                ++printed[k];
                break;
            }
        }
    }
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_PRODUCER_XREF_END version=1.0.0 appendCandidates=%llu appendPrinted=%llu constructorCandidates=%llu constructorPrinted=%llu failedChunks=%llu directCallsOnly=1 ownerItemAssociation=unverified backgroundWrites=0 noOverlay=1 globalRectDetour=0",
        static_cast<unsigned long long>(candidates[0]),
        static_cast<unsigned long long>(printed[0]),
        static_cast<unsigned long long>(candidates[1]),
        static_cast<unsigned long long>(printed[1]),
        static_cast<unsigned long long>(failedChunks));
    Emit(line);
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_COLOR_SOURCE_XREF_END version=1.0.0 sourceOwnersVerified=%u labelOwnerCandidates=%llu labelOwnerPrinted=%llu otherOwnerCandidates=%llu otherOwnerPrinted=%llu failedChunks=%llu candidateE8Only=1 indirectCallersNotCovered=1 itemOwnership=unverified no-hooks=1 no-writes=1",
        sourceOwnersVerified?1U:0U,
        static_cast<unsigned long long>(candidates[2]),
        static_cast<unsigned long long>(printed[2]),
        static_cast<unsigned long long>(candidates[3]),
        static_cast<unsigned long long>(printed[3]),
        static_cast<unsigned long long>(failedChunks));
    Emit(line);
}


// 0.1.99: the corrected direct E8-rel32 scan found no direct CALL to
// 0x843CA0. Search plausible address-taken references before inferring that
// the label-element owner is unreachable. A code-pointer/LEA hit is only a
// static reference; it does NOT prove a hovered item's renderer ownership.
// This reporter does no new detours, memory writes or process-wide data scan.
void ReportNativeHoverBackgroundIndirectOwnerRefs() noexcept {
    constexpr std::uintptr_t ownerRva=0x843CA0;
    constexpr std::array<std::uint8_t,7> expected{{
        0x48,0x89,0x6C,0x24,0x20,0x57,0x41}};
    const char* build=Context?D2RL::GetBuildName(Context):nullptr;
    std::array<std::uint8_t,7> entry{};
    if (!build || std::string_view(build)!="93847" || !Base || !ImageSize ||
        !ReadSafe(ownerRva,entry.data(),entry.size()) || entry!=expected) {
        Emit("LOOT_HOVER_BG_INDIRECT_OWNER_UNAVAILABLE version=1.0.0 reason=build-image-or-owner-fingerprint no-hooks=1 no-writes=1");
        return;
    }
    IMAGE_DOS_HEADER dos{};
    if (!ReadSafe(0,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew<0 || static_cast<std::size_t>(dos.e_lfanew)>0x100000) {
        Emit("LOOT_HOVER_BG_INDIRECT_OWNER_UNAVAILABLE version=1.0.0 reason=pe-dos-header no-hooks=1 no-writes=1");
        return;
    }
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadSafe(static_cast<std::uintptr_t>(dos.e_lfanew),&nt,sizeof(nt)) ||
        nt.Signature!=IMAGE_NT_SIGNATURE ||
        nt.FileHeader.NumberOfSections==0 || nt.FileHeader.NumberOfSections>96) {
        Emit("LOOT_HOVER_BG_INDIRECT_OWNER_UNAVAILABLE version=1.0.0 reason=pe-nt-header no-hooks=1 no-writes=1");
        return;
    }
    const auto sectionsRva=static_cast<std::uintptr_t>(dos.e_lfanew)+
        sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
    std::array<IMAGE_SECTION_HEADER,96> sections{};
    if (!ReadSafe(sectionsRva,sections.data(),
        static_cast<std::size_t>(nt.FileHeader.NumberOfSections)*sizeof(IMAGE_SECTION_HEADER))) {
        Emit("LOOT_HOVER_BG_INDIRECT_OWNER_UNAVAILABLE version=1.0.0 reason=section-headers no-hooks=1 no-writes=1");
        return;
    }
    constexpr std::size_t chunk=0x4000;
    constexpr std::uint64_t maxReported=24;
    std::vector<std::uint8_t> bytes{};
    try {bytes.resize(chunk+7);} catch (const std::exception&) {
        Emit("LOOT_HOVER_BG_INDIRECT_OWNER_UNAVAILABLE version=1.0.0 reason=scan-buffer-allocation no-hooks=1 no-writes=1");
        return;
    }
    std::uint64_t pointerMatches{},leaMatches{},failedChunks{};
    std::uint64_t printed{};
    const auto absoluteOwner=static_cast<std::uint64_t>(Base)+ownerRva;
    char line[360]{};
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_INDIRECT_OWNER_BEGIN version=1.0.0 owner=D2R+0x%llX directCallsPrevious=0 candidateScopes=.rdata,.data,.text patterns=absolute-VA64-or-REX-LEA-RIP scanReadOnly=1 no-hooks=1 no-writes=1",
        static_cast<unsigned long long>(ownerRva));
    Emit(line);
    for (std::size_t si=0;si<nt.FileHeader.NumberOfSections;++si) {
        const auto& sec=sections[si];
        const bool textSec=std::memcmp(sec.Name,".text",5)==0 &&
            (sec.Characteristics&IMAGE_SCN_MEM_EXECUTE)!=0;
        const bool pointerSec=std::memcmp(sec.Name,".rdata",6)==0 ||
            std::memcmp(sec.Name,".data",5)==0;
        if (!textSec && !pointerSec)continue;
        const auto start=static_cast<std::uintptr_t>(sec.VirtualAddress);
        const auto length=static_cast<std::size_t>(sec.Misc.VirtualSize);
        if (!start || start>=ImageSize || length<8 ||
            length>0x4000000U || length>ImageSize-start) {
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_BG_INDIRECT_OWNER_SECTION_SKIPPED version=1.0.0 section=%s reason=invalid-or-oversized-section no-hooks=1",
                textSec?".text":std::memcmp(sec.Name,".rdata",6)==0?".rdata":".data");
            Emit(line);
            continue;
        }
        const char* sectionName=textSec?".text":
            std::memcmp(sec.Name,".rdata",6)==0?".rdata":".data";
        const auto end=start+length;
        for (auto at=start;at<end;at+=chunk) {
            const auto actual=std::min<std::size_t>(chunk+7,end-at);
            if (actual<8 || !ReadSafe(at,bytes.data(),actual)) {
                ++failedChunks;
                continue;
            }
            // +7 overlap handles every possible eight-byte pointer even at
            // chunk boundaries. The last seven section bytes cannot start a
            // complete pointer and are not counted as candidates.
            const auto starts=std::min<std::size_t>(chunk,actual-7);
            for (std::size_t i=0;i<starts;++i) {
                const auto site=at+i;
                if (pointerSec) {
                    std::uint64_t value{};
                    std::memcpy(&value,bytes.data()+i,sizeof(value));
                    if (value!=absoluteOwner)continue;
                    ++pointerMatches;
                    if (printed>=maxReported)continue;
                    std::snprintf(line,sizeof(line),
                        "LOOT_HOVER_BG_INDIRECT_OWNER_REF version=1.0.0 kind=absolute-pointer section=%s siteRva=D2R+0x%llX target=D2R+0x%llX interpretation=possible-address-taken-reference-not-owner-proof",
                        sectionName,
                        static_cast<unsigned long long>(site),
                        static_cast<unsigned long long>(ownerRva));
                } else {
                    // REX.W + LEA r64,[RIP+disp32]. We check the ModRM
                    // addressing form explicitly, rather than treating any
                    // appearance of E8/8D as instruction-aligned proof.
                    if ((bytes[i]&0xF8)!=0x48 || bytes[i+1]!=0x8D ||
                        (bytes[i+2]&0xC7)!=0x05)continue;
                    std::int32_t displacement{};
                    std::memcpy(&displacement,bytes.data()+i+3,sizeof(displacement));
                    const auto destination=static_cast<std::int64_t>(site+7)+
                        static_cast<std::int64_t>(displacement);
                    if (destination!=static_cast<std::int64_t>(ownerRva))continue;
                    ++leaMatches;
                    if (printed>=maxReported)continue;
                    std::snprintf(line,sizeof(line),
                        "LOOT_HOVER_BG_INDIRECT_OWNER_REF version=1.0.0 kind=rex-rip-lea section=.text siteRva=D2R+0x%llX target=D2R+0x%llX interpretation=candidate-address-taken-reference-not-instruction-alignment-or-owner-proof",
                        static_cast<unsigned long long>(site),
                        static_cast<unsigned long long>(ownerRva));
                }
                Emit(line);
                ++printed;
            }
        }
    }
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BG_INDIRECT_OWNER_END version=1.0.0 pointerMatches=%llu leaMatches=%llu printed=%llu failedChunks=%llu directCallsPrevious=0 noHitDoesNotProveUnused=1 itemIdentity=unverified backgroundWrites=0 no-hooks=1 no-overlay=1",
        static_cast<unsigned long long>(pointerMatches),
        static_cast<unsigned long long>(leaMatches),
        static_cast<unsigned long long>(printed),
        static_cast<unsigned long long>(failedChunks));
    Emit(line);
}

// 0.1.99: inspect the caller that supplies an element to the native UI
// append routine, and the common native render-element constructor itself.
// The earlier direct-call scan proves only possible code relationships,
// not that a particular record belongs to a hovered item. Keep this
// diagnostic entirely static. Never detour the shared rectangle renderer,
// change the render-element array or borrow a pointer across UI frames.
void ReportNativeHoverBackgroundElementSource() noexcept {
    const char* build=Context?D2RL::GetBuildName(Context):nullptr;
    if (!build || std::string_view(build)!="93847" || !Base || !ImageSize) {
        Emit("LOOT_HOVER_BG_ELEMENT_SOURCE_UNAVAILABLE version=1.0.0 reason=build-or-image no-hooks=1 no-writes=1");
        return;
    }
    struct Site final {
        const char* name;
        std::uintptr_t callRva;
        std::uintptr_t targetRva;
        std::array<std::uint8_t,5> expected;
        std::uintptr_t windowRva;
        std::size_t windowSize;
    };
    // Independent MSVC-compatible byte fingerprints from the user's 0.1.99
    // build-93847 capture. A call target by itself is NOT type information.
    constexpr std::array<Site,3> sites{{
        {"append-owner",0x843D87,0x880160,{{0xE8,0xD4,0xC3,0x03,0x00}},0x843BE0,0x1B0},
        {"constructor-owner",0x87FF7B,0x8D9EB0,{{0xE8,0x30,0x9F,0x05,0x00}},0x87FEE0,0x130},
        {"append-constructor",0x8801DD,0x8D9EB0,{{0xE8,0xCE,0x9C,0x05,0x00}},0x8D9EB0,0x300}
    }};
    char line[440]{};
    for (const auto& site:sites) {
        std::array<std::uint8_t,5> observed{};
        if (!ReadSafe(site.callRva,observed.data(),observed.size()) ||
            observed!=site.expected) {
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_BG_ELEMENT_SOURCE_UNAVAILABLE version=1.0.0 path=%s reason=call-fingerprint-mismatch expectedTarget=D2R+0x%llX no-hooks=1 no-writes=1",
                site.name,static_cast<unsigned long long>(site.targetRva));
            Emit(line);
            continue;
        }
        std::int32_t relative{};
        std::memcpy(&relative,observed.data()+1,sizeof(relative));
        const auto target=static_cast<std::int64_t>(site.callRva+5)+
            static_cast<std::int64_t>(relative);
        if (target!=static_cast<std::int64_t>(site.targetRva)) {
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_BG_ELEMENT_SOURCE_UNAVAILABLE version=1.0.0 path=%s reason=decoded-target-mismatch no-hooks=1 no-writes=1",
                site.name);
            Emit(line);
            continue;
        }
        if (site.windowRva>=ImageSize ||
            site.windowSize>ImageSize-site.windowRva ||
            site.windowSize>0x400) {
            Emit("LOOT_HOVER_BG_ELEMENT_SOURCE_UNAVAILABLE version=1.0.0 reason=window-bounds no-hooks=1 no-writes=1");
            continue;
        }
        std::array<std::uint8_t,0x400> bytes{};
        if (!ReadSafe(site.windowRva,bytes.data(),site.windowSize)) {
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_BG_ELEMENT_SOURCE_UNAVAILABLE version=1.0.0 path=%s reason=window-read no-hooks=1 no-writes=1",
                site.name);
            Emit(line);
            continue;
        }
        DWORD64 imageBase{};
        const auto* unwind=RtlLookupFunctionEntry(
            static_cast<DWORD64>(Base+site.callRva),&imageBase,nullptr);
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_BG_ELEMENT_SOURCE_READY version=1.0.0 path=%s callRva=D2R+0x%llX target=D2R+0x%llX fingerprint=verified unwindAvailable=%u unwindBeginRva=D2R+0x%llX unwindEndRva=D2R+0x%llX itemIdentity=unverified nativeObjectOwnership=unverified no-hooks=1 no-writes=1",
            site.name,
            static_cast<unsigned long long>(site.callRva),
            static_cast<unsigned long long>(site.targetRva),
            unwind?1U:0U,
            unwind?static_cast<unsigned long long>(unwind->BeginAddress):0ULL,
            unwind?static_cast<unsigned long long>(unwind->EndAddress):0ULL);
        Emit(line);
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_BG_ELEMENT_SOURCE_WINDOW_BEGIN version=1.0.0 path=%s startRva=D2R+0x%llX bytes=%zu nativeStaticCodeOnly=1 instructionAligned=unverified no-hooks=1",
            site.name,static_cast<unsigned long long>(site.windowRva),
            site.windowSize);
        Emit(line);
        for (std::size_t offset=0;offset<site.windowSize;offset+=16) {
            char hex[49]{};
            const auto count=std::min<std::size_t>(16,site.windowSize-offset);
            for (std::size_t b=0;b<count;++b)
                std::snprintf(hex+b*3,sizeof(hex)-b*3,"%02X ",
                    unsigned(bytes[offset+b]));
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_BG_ELEMENT_SOURCE_BYTES path=%s rva=D2R+0x%llX hex='%s'",
                site.name,
                static_cast<unsigned long long>(site.windowRva+offset),hex);
            Emit(line);
        }
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_BG_ELEMENT_SOURCE_WINDOW_END version=1.0.0 path=%s next=offline-disassemble-argument-origin-and-element-color-initialization no-hooks=1 no-writes=1 no-overlay=1",
            site.name);
        Emit(line);
    }
}

// 0.1.99: the previously captured native constructor copies a large input
// descriptor into an element. Its R9 input is preserved in RBX, and 0xE8
// within that input is copied to element+0x168 (the value fed to the native
// rectangle renderer). This validates transfer, not item/tooltip ownership.
// Read only executable bytes, and fail closed on any version mismatch.
void ReportNativeHoverBackgroundColorTransfer() noexcept {
    const char* build=Context?D2RL::GetBuildName(Context):nullptr;
    if (!build || std::string_view(build)!="93847" || !Base || !ImageSize) {
        Emit("LOOT_HOVER_BG_COLOR_TRANSFER_UNAVAILABLE version=1.0.0 reason=build-or-image no-hooks=1 no-writes=1");
        return;
    }
    constexpr std::array<std::uint8_t,9> registersExpected{{
        0x49,0x8B,0xD9,0x4D,0x8B,0xF8,0x48,0x8B,0xF9}};
    constexpr std::array<std::uint8_t,16> copyExpected{{
        0x48,0x8D,0x89,0x80,0x00,0x00,0x00,0x0F,
        0x10,0x03,0x48,0x8D,0x9B,0x80,0x00,0x00}};
    constexpr std::array<std::uint8_t,16> copyLaneExpected{{
        0x0F,0x11,0x41,0x80,0x0F,0x10,0x4B,0x90,
        0x0F,0x11,0x49,0x90,0x0F,0x10,0x43,0xA0}};
    std::array<std::uint8_t,9> registers{};
    std::array<std::uint8_t,16> copy{},lane{};
    if (!ReadSafe(0x8D9EE1,registers.data(),registers.size()) ||
        !ReadSafe(0x8D9F50,copy.data(),copy.size()) ||
        !ReadSafe(0x8D9F61,lane.data(),lane.size()) ||
        registers!=registersExpected || copy!=copyExpected ||
        lane!=copyLaneExpected) {
        Emit("LOOT_HOVER_BG_COLOR_TRANSFER_UNAVAILABLE version=1.0.0 reason=constructor-copy-fingerprint no-hooks=1 no-writes=1");
        return;
    }
    Emit("LOOT_HOVER_BG_COLOR_TRANSFER_READY version=1.0.0 constructor=D2R+0x8D9EB0 sourceRegister=R9 sourceCopyBase=R9+0x0 sourceColorOffset=0xE8 destinationRegister=RCX destinationCopyBase=RCX+0x80 destinationColorOffset=0x168 stride=0x2E8 transfer=original-native-structure-copy itemIdentity=unverified backgroundRGBAType=not-runtime-verified writes=0 hooks=0 overlays=0");
}

// 0.1.99: focus on the actual item-label native builder rather than the
// speculative 0x843CA0 UI-element owner. The SoE V1/V2 callbacks establish
// the item identity at D2R+0xC0420, but V3 has observed zero overlapping
// glyph/painter calls: no native render-element ownership is established.
// SoE hooks the builder entry when present. Its live first bytes may be a
// loader bridge rather than the pristine bytes of build 93847. NEVER infer a
// native object layout from a patched entry or add another builder detour.
// Read a few bounded code windows for offline native-code analysis only.
void ReportHiddenHoverNativeBuilderPath() noexcept {
    constexpr std::uintptr_t builder=InWorldFormatterRva;
    constexpr std::uintptr_t nativeCall=0xC0F67;
    constexpr std::uintptr_t nativeReturn=nativeCall+5;
    const char* build=Context?D2RL::GetBuildName(Context):nullptr;
    if (!build || std::string_view(build)!="93847" || !Base || !ImageSize) {
        Emit("LOOT_HOVER_BUILDER_UNAVAILABLE version=1.0.0 reason=build-or-image-unavailable no-hook=1 no-writes=1");
        return;
    }
    std::array<std::uint8_t,16> entry{};
    std::array<std::uint8_t,5> call{};
    if (!ReadSafe(builder,entry.data(),entry.size()) ||
        !ReadSafe(nativeCall,call.data(),call.size())) {
        Emit("LOOT_HOVER_BUILDER_UNAVAILABLE version=1.0.0 reason=live-builder-or-caller-read-failed no-hook=1 no-writes=1");
        return;
    }
    std::int32_t displacement{};
    std::memcpy(&displacement,call.data()+1,sizeof(displacement));
    const auto decodedTarget=static_cast<std::int64_t>(nativeReturn)+
        static_cast<std::int64_t>(displacement);
    const bool callDecoded=call[0]==0xE8 && decodedTarget>=0 &&
        static_cast<std::uint64_t>(decodedTarget)<ImageSize;
    const bool entryPristine=entry==ExpectedInWorldFormatter;
    // Never classify a loader's bridge format by guessed opcodes. We know
    // which backend was attached but not that an arbitrary entry prefix is
    // the original native function or a D2R render-element pointer.
    const auto backend=InWorldMode.load(std::memory_order_acquire);
    const char* mode=backend==InWorldBackend::SoEInterop?"soe-interop":
        backend==InWorldBackend::StandaloneIdentity?"standalone":
        backend==InWorldBackend::Blocked?"blocked":"pending";
    char line[480]{};
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BUILDER_READY version=1.0.0 build=93847 builder=D2R+0x%llX backend=%s entryPristine=%u caller=D2R+0x%llX callerIsE8=%u decodedTarget=D2R+0x%llX targetInsideImage=%u itemId=%u itemIdentity=SoE-V1-or-standalone-only UIElementIdentity=unverified nativeBackgroundWrites=0 newHooks=0 overlay=0",
        static_cast<unsigned long long>(builder),mode,entryPristine?1U:0U,
        static_cast<unsigned long long>(nativeCall),call[0]==0xE8?1U:0U,
        static_cast<unsigned long long>(decodedTarget>=0?decodedTarget:0),
        callDecoded?1U:0U,HoverDiffItemId.load(std::memory_order_acquire));
    Emit(line);
    char entryHex[16*3+1]{};
    for (std::size_t i=0;i<entry.size();++i)
        std::snprintf(entryHex+3*i,sizeof(entryHex)-3*i,"%02X ",unsigned(entry[i]));
    std::snprintf(line,sizeof(line),
        "LOOT_HOVER_BUILDER_ENTRY version=1.0.0 rva=D2R+0x%llX bytes='%s' patchedByOwnerPossible=%u never-assume-pristine=1",
        static_cast<unsigned long long>(builder),entryHex,
        entryPristine?0U:1U);
    Emit(line);
    if (!callDecoded) {
        Emit("LOOT_HOVER_BUILDER_STOP version=1.0.0 reason=caller-not-a-valid-E8-target no-fallback-hook=1");
        return;
    }
    // These windows inspect the item-label builder and the path back to its
    // observed caller. They are NOT guaranteed instruction-aligned; code
    // bytes alone cannot prove that the game's render record is the item.
    struct Window {const char* name;std::uintptr_t rva;std::size_t size;};
    constexpr std::array<Window,4> windows{{
        {"builder-entry-and-continuation",builder,0x300},
        {"builder-middle-early",0xC0720,0x300},
        {"builder-middle-late",0xC0A20,0x300},
        {"builder-return-neighborhood",0xC0D20,0x300},
    }};
    std::array<std::uint8_t,0x300> bytes{};
    for (const auto& window:windows) {
        if (!ReadSafe(window.rva,bytes.data(),window.size)) {
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_BUILDER_WINDOW_UNAVAILABLE version=1.0.0 path=%s startRva=D2R+0x%llX reason=bounded-read-failed no-hook=1",
                window.name,static_cast<unsigned long long>(window.rva));
            Emit(line);continue;
        }
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_BUILDER_WINDOW_BEGIN version=1.0.0 path=%s startRva=D2R+0x%llX bytes=%zu staticCodeOnly=1 instructionAligned=unverified no-hook=1",
            window.name,static_cast<unsigned long long>(window.rva),window.size);
        Emit(line);
        for (std::size_t offset=0;offset<window.size;offset+=16) {
            char hex[16*3+1]{};
            for (std::size_t i=0;i<16;++i)
                std::snprintf(hex+3*i,sizeof(hex)-3*i,"%02X ",
                    unsigned(bytes[offset+i]));
            std::snprintf(line,sizeof(line),
                "LOOT_HOVER_BUILDER_BYTES path=%s rva=D2R+0x%llX hex='%s'",
                window.name,
                static_cast<unsigned long long>(window.rva+offset),hex);
            Emit(line);
        }
        std::snprintf(line,sizeof(line),
            "LOOT_HOVER_BUILDER_WINDOW_END version=1.0.0 path=%s no-hook=1 no-writes=1",
            window.name);
        Emit(line);
    }
    Emit("LOOT_HOVER_BUILDER_END version=1.0.0 next=offline-trace-formatter-result-to-native-render-record builderItemIdentity=observed renderElementOwner=not-proven UIObjectPointersRetained=0 nativeColorWrites=0 globalRectDetour=0 overlay=0");
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
    if(request->automatic) TraceLootLatency(request->code,request->unitId,
        LootLatencyStage::NativeSound,
        result?"native-player-returned-nonnull":"native-player-returned-null");
    char codeText[5]{};
    CodeText(request->code,codeText);
    char message[320]{};
    std::snprintf(message,sizeof(message),
        "LOOT_SOUND_PLAY version=1.0.0 trigger=%s code='%s' unitId=%u name='%s' engineReturned=%u gameThread=%lu note=return-not-audibility-proof",
        request->automatic?(request->hiddenHover?"first-hidden-hover":"first-alt-ground-label"):"manual-test",codeText,request->unitId,
        request->name.data(),result?1U:0U,static_cast<unsigned long>(GetCurrentThreadId()));
    if(logger) {
        logger->LogInfo(message);
        CaptureLine(message);
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
    if(automatic)TraceLootLatency(code,unitId,
        LootLatencyStage::QueueSound,"before-runOnGameThread");
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
    TraceLootLatency(code,unitId,LootLatencyStage::MatchSound,
        hiddenHover?"matched-soe-label":"matched-formatter-label",
        0,item.quality,item.itemLevel);
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
    ObserveGroundPropertyCarriedSdk(item); // optional F8 evidence, read-only
    ObserveSocketProbeCarriedSdk(item);
    ObserveEtherealProbeCarriedSdk(item);
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
                { ObserveGroundPropertyCarriedSdk(&info);
                  ObserveSocketProbeCarriedSdk(&info);
                  ObserveEtherealProbeCarriedSdk(&info);
                  ForgetMinimapProjectionItem(info.runtimeId);
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
    const auto mode=ActiveGeometryMode.load(std::memory_order_acquire);
    if ((mode!=GeometryMode::Off && mode!=GeometryMode::Rules) ||
        RenameArmed.load(std::memory_order_acquire) ||
        CodeRenameArmed.load(std::memory_order_acquire) ||
        CodeBridgeArmed.load(std::memory_order_acquire)) {
        Emit("LOOT_FILTER_AUTO_INACTIVE reason=conflicting-manual-probe");
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
// This observer is opt-in, calls the original exactly once and never performs
// file IO from the hook. Label writes require the independent rename-arm gate.
std::uint8_t __fastcall HookLabelFormatter(
    void* unit, void* dest, void* record, std::uint32_t flags,
    std::uint64_t stack5, std::uint64_t stack6) noexcept {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const int index = ActivePhase.load(std::memory_order_acquire);
    const bool isSourceCall =
        caller == Base + LabelFormatterCallRva + DirectCallBytes ||
        caller == Base + LabelFormatterSecondCallRva + DirectCallBytes;
    const auto destAddress = reinterpret_cast<std::uintptr_t>(dest);
    const auto recordAddress = reinterpret_cast<std::uintptr_t>(record);
    const bool paired = record && dest &&
        recordAddress <= static_cast<std::uintptr_t>(-1) - 0x24 &&
        destAddress == recordAddress + 0x24;
    FormatterObservation* reserved{};
    std::size_t rowIndex = FormatterSlotsPerPhase;
    if (index >= 0 &&
        static_cast<std::size_t>(index) < MaximumPhases &&
        GetTickCount64() < Deadline.load(std::memory_order_relaxed)) {
        if (PhasesMutex.try_lock()) {
            if (ActivePhase.load(std::memory_order_acquire) == index &&
                static_cast<std::size_t>(index) < PhaseCount) {
                auto& f = Phases[static_cast<std::size_t>(index)].formatter;
                ++f.seen;
                if (!f.firstReturnAddress) f.firstReturnAddress = caller;
                if (isSourceCall) ++f.sourceSiteHits;
                if (paired) ++f.pairedHits;
                if (isSourceCall && paired) {
                    ++f.calls;
                    ++f.sourceCallHits;
                    for (std::size_t i = 0; i < f.rowCount; ++i) {
                        if (f.rows[i].nativeUnit == reinterpret_cast<std::uintptr_t>(unit) &&
                            f.rows[i].record == recordAddress) {
                            ++f.rows[i].hits;
                            rowIndex = i;
                            break;
                        }
                    }
                    if (rowIndex == FormatterSlotsPerPhase) {
                        if (f.rowCount < FormatterSlotsPerPhase) {
                            rowIndex = f.rowCount++;
                            auto& row = f.rows[rowIndex];
                            row.claimed = true;
                            row.nativeUnit = reinterpret_cast<std::uintptr_t>(unit);
                            row.record = recordAddress;
                            row.dest = destAddress;
                            row.caller = caller;
                            row.fourthArg = flags;
                            row.fifthArg = stack5;
                            row.sixthArg = stack6;
                            row.hits = 1;
                            row.threadId = GetCurrentThreadId();
                            reserved = &row;
                        } else {
                            ++f.skipped;
                        }
                    }
                }
            }
            PhasesMutex.unlock();
        } else FormatterContention.fetch_add(1, std::memory_order_relaxed);
    }
    FormatterObservation temp{};
    if (reserved) {
        SIZE_T copied{};
        temp.preOk = ReadProcessMemory(GetCurrentProcess(), record,
            temp.pre.data(), temp.pre.size(), &copied) &&
            copied == temp.pre.size();
        copied = 0;
        temp.unitOk = unit && ReadProcessMemory(GetCurrentProcess(), unit,
            temp.nativeFirst6, sizeof(temp.nativeFirst6), &copied) &&
            copied == sizeof(temp.nativeFirst6);
    }
    // No changes to the six arguments; no extra native calls or synthetic UI.
    const auto result = OriginalLabelFormatter(
        unit, dest, record, flags, stack5, stack6);
    ObserveHoverRoute(HoverStage::Formatter,caller,unit,dest,record,flags);
    FormatterCalls.fetch_add(1, std::memory_order_relaxed);
    TryRenameDivineLabel(unit, record, isSourceCall, paired, result);
    TryCodeRenameGroundLabel(unit, record, isSourceCall, paired, result);
    if(isSourceCall && paired && result && unit && OriginalGetItemCode) {
        std::array<std::uint32_t,4> identity{};
        SIZE_T bytes{};
        if(ReadProcessMemory(GetCurrentProcess(),unit,identity.data(),
                sizeof(identity),&bytes) && bytes==sizeof(identity) &&
            identity[0]==4 && identity[2] && identity[3]==3)
            TraceLootLatency(OriginalGetItemCode(unit),identity[2],
                LootLatencyStage::ObserveFormatter,"post-native-formatter");
    }
    RememberGroundIdentity(unit, record, isSourceCall, paired, result);
    ExamineGroundSoundCandidate(unit,record,isSourceCall,paired,result);
    if (reserved) {
        SIZE_T copied{};
        temp.postOk = ReadProcessMemory(GetCurrentProcess(), record,
            temp.post.data(), temp.post.size(), &copied) &&
            copied == temp.post.size();
        // One-shot resolver after native formatter, with the same verified
        // pointer and ID pair as the 0.1.16 capture. Never mutate this unit.
        ProbeFormatterItemCode(unit, isSourceCall && paired && result != 0, temp);
        // Only a freshly reserved row is modified. Start/Clear can race an
        // in-flight callback; check the same pointer identity before storing.
        if (PhasesMutex.try_lock()) {
            if (ActivePhase.load(std::memory_order_acquire) == index &&
                static_cast<std::size_t>(index) < PhaseCount &&
                rowIndex < Phases[static_cast<std::size_t>(index)].formatter.rowCount) {
                auto& row = Phases[static_cast<std::size_t>(index)].formatter.rows[rowIndex];
                if (row.nativeUnit == reinterpret_cast<std::uintptr_t>(unit) &&
                    row.record == recordAddress) {
                    row.pre = temp.pre;
                    row.post = temp.post;
                    std::memcpy(row.nativeFirst6,temp.nativeFirst6,
                        sizeof(row.nativeFirst6));
                    row.preOk = temp.preOk;
                    row.postOk = temp.postOk;
                    row.unitOk = temp.unitOk;
                    row.codeAttempted = temp.codeAttempted;
                    row.codeGuardPassed = temp.codeGuardPassed;
                    row.codeValid = temp.codeValid;
                    row.codeValue = temp.codeValue;
                    row.result = result;
                }
            }
            PhasesMutex.unlock();
        } else FormatterContention.fetch_add(1, std::memory_order_relaxed);
    }
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
    Emit("LOOT_FORMATTER_OBSERVER_READY version=1.0.0 hook=D2R+0x1FA9F0 sourceCalls=D2R+0x15171E5,D2R+0x1517783 expectedReturnRvas=0x15171EA,0x1517788 sixArgsForwarded=1 labelWrites=opt-in-name-and-code-rename nativeCodeWrites=loader-managed-hook-only");
}

// Escape a *candidate*, bounded ASCII/UTF-8 byte span. No dereference or
// guessing at item ownership. Non-ASCII bytes are represented as \xHH so
// the capture file stays printable and retains the exact raw byte values.
bool CandidateText(const std::array<std::uint8_t, RecordBytes>& record,
                   char (&output)[CandidateTextMaximum * 4 + 1]) noexcept {
    std::size_t used = 0;
    bool terminated = false;
    for (std::size_t i = 0; i < CandidateTextMaximum; ++i) {
        const auto ch = record[CandidateTextOffset + i];
        if (!ch) { terminated = true; break; }
        if (ch >= 0x20 && ch <= 0x7e && ch != '\\' && ch != '\'') {
            output[used++] = static_cast<char>(ch);
        } else if (ch == '\n' || ch == '\r' || ch == '\t' || ch == '\\' || ch == '\'') {
            output[used++] = '\\';
            output[used++] = ch == '\n' ? 'n' : ch == '\r' ? 'r' : ch == '\t' ? 't' : static_cast<char>(ch);
        } else {
            static constexpr char hex[] = "0123456789ABCDEF";
            output[used++] = '\\'; output[used++] = 'x';
            output[used++] = hex[ch >> 4U]; output[used++] = hex[ch & 15U];
        }
    }
    output[used] = 0;
    return terminated && used != 0;
}

void CountCandidateText(CollectionSample& sample,
                        const std::array<std::uint8_t, RecordBytes>& record) noexcept {
    ++sample.textSamples;
    char text[CandidateTextMaximum * 4 + 1]{};
    if (!CandidateText(record, text)) { ++sample.invalidTextSamples; return; }
    for (std::size_t i = 0; i < sample.variantCount; ++i) {
        if (std::strcmp(sample.variants[i].escaped, text) == 0) {
            ++sample.variants[i].samples;
            return;
        }
    }
    if (sample.variantCount >= CandidateTextVariants) { ++sample.variantOverflow; return; }
    auto& variant = sample.variants[sample.variantCount++];
    std::memcpy(variant.escaped, text, std::strlen(text) + 1);
    variant.samples = 1;
}

// Bounded, non-recursive reads only; scanning unused slots may observe stale
// record bytes and is not a visibility or item-identity determination.
void SurveySlots(void* arg3, SlotSurvey& survey, std::uint64_t atHit) noexcept {
    survey = SlotSurvey{};
    survey.attempted = true;
    survey.atHelperHit = atHit;
    if (!arg3) return;
    const auto start = reinterpret_cast<std::uintptr_t>(arg3);
    for (std::size_t i = 0; i < CandidateSlotCount; ++i) {
        constexpr auto Max = static_cast<std::uintptr_t>(-1);
        const auto offset = i * RecordBytes;
        if (start > Max - offset) break;
        const auto address = start + offset;
        std::array<std::uint8_t, RecordBytes> bytes{};
        SIZE_T copied{};
        if (!ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const void*>(address), bytes.data(),
                bytes.size(), &copied) || copied != bytes.size())
            continue;
        auto& slot = survey.slots[i];
        slot.readable = true;
        ++survey.readable;
        std::memcpy(slot.prefix.data(), bytes.data(), slot.prefix.size());
        std::memcpy(slot.text.data(), bytes.data() + CandidateTextOffset,
            slot.text.size());
        const auto stop = std::find(slot.text.begin(), slot.text.end(), 0);
        slot.terminated = stop != slot.text.end();
        if (!slot.terminated || stop - slot.text.begin() < 2) continue;
        bool hasLetter = false;
        for (auto ch = slot.text.begin(); ch != stop; ++ch) {
            hasLetter |= (*ch >= 'A' && *ch <= 'Z') ||
                         (*ch >= 'a' && *ch <= 'z');
        }
        if (hasLetter) ++survey.textCandidates;
    }
}

bool SlotName(const SlotCandidate& slot,
              char (&out)[CandidateTextMaximum * 4 + 1]) noexcept {
    if (!slot.readable || !slot.terminated) return false;
    std::size_t used{};
    std::size_t count{};
    bool hasLetter = false;
    for (const auto ch : slot.text) {
        if (!ch) break;
        ++count;
        hasLetter |= (ch >= 'A' && ch <= 'Z') ||
                     (ch >= 'a' && ch <= 'z');
        if (ch >= 0x20 && ch <= 0x7e && ch != '\\' && ch != '\'')
            out[used++] = static_cast<char>(ch);
        else if (ch == '\n' || ch == '\r' || ch == '\t' || ch == '\\' || ch == '\'') {
            out[used++] = '\\';
            out[used++] = ch == '\n' ? 'n' : ch == '\r' ? 'r' : ch == '\t' ? 't' : static_cast<char>(ch);
        } else {
            static constexpr char hex[] = "0123456789ABCDEF";
            out[used++] = '\\'; out[used++] = 'x';
            out[used++] = hex[ch >> 4U]; out[used++] = hex[ch & 15U];
        }
    }
    out[used] = 0;
    return count >= 2 && hasLetter;
}

// Optional loader-tracked inline hook. The original function is always called
// exactly once. The hook never changes arguments, item state, name, or visibility.
std::uint64_t __fastcall HookCollectionHelper(
    void* arg1, void* arg2, void* arg3, std::uint64_t limit) noexcept {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const int beforeIndex = ActivePhase.load(std::memory_order_acquire);
    bool firstPreClaim = false;
    bool firstPreOk = false;
    bool firstPreSurveyOk = false;
    std::array<std::uint8_t, RecordBytes> firstPre{};
    if (beforeIndex >= 0 && static_cast<std::size_t>(beforeIndex) < MaximumPhases &&
        GetTickCount64() < Deadline.load(std::memory_order_relaxed) &&
        !BeforeClaimed[static_cast<std::size_t>(beforeIndex)].exchange(true,
            std::memory_order_acq_rel)) {
        firstPreClaim = true;
        // Claim and record the first bounded full-slot survey before the
        // original helper runs; do not hold the phase mutex across the call.
        if (PhasesMutex.try_lock()) {
            if (ActivePhase.load(std::memory_order_acquire) == beforeIndex &&
                static_cast<std::size_t>(beforeIndex) < PhaseCount) {
                SurveySlots(arg3,
                    Phases[static_cast<std::size_t>(beforeIndex)]
                        .collection.firstBeforeSlots, 1);
                firstPreSurveyOk = true;
            }
            PhasesMutex.unlock();
        }
        SIZE_T copied{};
        if (arg3) {
            firstPreOk = ReadProcessMemory(GetCurrentProcess(), arg3, firstPre.data(),
                firstPre.size(), &copied) && copied == firstPre.size();
        }
    }
    const auto result = OriginalCollectionHelper(arg1, arg2, arg3, limit);
    CollectionTotal.fetch_add(1, std::memory_order_relaxed);
    const int index = ActivePhase.load(std::memory_order_acquire);
    if (index < 0 || static_cast<std::size_t>(index) >= MaximumPhases) return result;
    if (GetTickCount64() >= Deadline.load(std::memory_order_relaxed)) {
        int expected = index;
        ActivePhase.compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
        return result;
    }
    if (!PhasesMutex.try_lock()) {
        CollectionContended.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    if (ActivePhase.load(std::memory_order_acquire) == index &&
        static_cast<std::size_t>(index) < PhaseCount) {
        auto& sample = Phases[static_cast<std::size_t>(index)].collection;
        if (sample.hits == 0) {
            sample.firstLimit = limit;
            sample.minimumLimit = limit;
            sample.maximumLimit = limit;
            sample.firstCaller = caller;
            sample.firstArg1 = reinterpret_cast<std::uintptr_t>(arg1);
            sample.firstArg2 = reinterpret_cast<std::uintptr_t>(arg2);
            sample.firstArg3 = reinterpret_cast<std::uintptr_t>(arg3);
            sample.firstThread = GetCurrentThreadId();
            if (firstPreClaim && beforeIndex == index) {
                sample.beforeArg3Ok = firstPreOk;
                if (firstPreOk) sample.beforeArg3 = firstPre;
                if (!firstPreSurveyOk) ++sample.surveyClaimFailures;
            } else {
                ++sample.preClaimFailures;
                ++sample.surveyClaimFailures;
            }
            SurveySlots(arg3, sample.firstAfterSlots, 1);
            sample.lastAfterSlots = sample.firstAfterSlots;
            sample.surveyUpdates = 1;
            // Direct guarded reads of arg1 and arg3 only. No pointer chasing.
            SIZE_T copied{};
            if (arg1) {
                sample.arg1SnapshotOk = ReadProcessMemory(GetCurrentProcess(), arg1,
                    sample.arg1Snapshot.data(), sizeof(sample.arg1Snapshot),
                    &copied) && copied == sizeof(sample.arg1Snapshot);
            }
            copied = 0;
            if (arg3) {
                sample.afterArg3Ok = ReadProcessMemory(GetCurrentProcess(), arg3,
                    sample.afterArg3.data(), sample.afterArg3.size(),
                    &copied) && copied == sample.afterArg3.size();
                if (sample.afterArg3Ok) {
                    std::memcpy(sample.arg3Snapshot.data(),
                        sample.afterArg3.data(), sizeof(sample.arg3Snapshot));
                    sample.arg3SnapshotOk = true;
                } else { // retain short-read compatibility with the 0.1.9 census
                    copied = 0;
                    sample.arg3SnapshotOk = ReadProcessMemory(GetCurrentProcess(), arg3,
                        sample.arg3Snapshot.data(), sizeof(sample.arg3Snapshot),
                        &copied) && copied == sizeof(sample.arg3Snapshot);
                }
            }
            PVOID frames[8]{};
            sample.stackCount = CaptureStackBackTrace(0, 8, frames, nullptr);
            for (std::uint32_t n = 0; n < sample.stackCount; ++n)
                sample.stack[n] = reinterpret_cast<std::uintptr_t>(frames[n]);
        }
        ++sample.hits;
        // At most one 32-record scan per 79 collection-helper calls.
        // Initial survey is collected above (hit 1).
        if (arg3 && sample.hits > 1 &&
            sample.hits % SlotSamplingInterval == 0) {
            SurveySlots(arg3, sample.lastAfterSlots, sample.hits);
            ++sample.surveyUpdates;
            if (sample.lastAfterSlots.readable != CandidateSlotCount)
                ++sample.surveyPeriodicFailures;
        }
        // Sample at a non-power-of-two interval to avoid aliasing with a
        // possible two-entry alternation. At most ~69 guarded reads per 8s.
        if (arg3 && (sample.hits == 1 ||
                     sample.hits % TextSamplingInterval == 0)) {
            if (sample.hits == 1 && sample.afterArg3Ok) {
                CountCandidateText(sample, sample.afterArg3);
            } else {
                std::array<std::uint8_t, RecordBytes> observed{};
                SIZE_T copied{};
                if (ReadProcessMemory(GetCurrentProcess(), arg3, observed.data(),
                        observed.size(), &copied) && copied == observed.size()) {
                    CountCandidateText(sample, observed);
                } else {
                    ++sample.candidateReadFailures;
                }
            }
        }
        sample.lastLimit = limit;
        sample.minimumLimit = std::min(sample.minimumLimit, limit);
        sample.maximumLimit = std::max(sample.maximumLimit, limit);
        sample.lastCaller = caller;
    }
    PhasesMutex.unlock();
    return result;
}

void ArmCollectionObserver() noexcept {
    if (CollectionHookInstalled.load(std::memory_order_acquire)) {
        Emit("LOOT_COLLECTION_OBSERVER_READY already-installed=1");
        return;
    }
    if (!Context || !Base || !ImageSize ||
        !D2RL::GetBuildName(Context) ||
        std::string_view(D2RL::GetBuildName(Context)) != "93847") {
        Emit("LOOT_COLLECTION_REFUSED incorrect-build-or-image");
        return;
    }
    std::array<std::uint8_t, 5> call{};
    if (!ReadSafe(CollectionCallRva, call.data(), call.size()) ||
        call != ExpectedCollectionCaller) {
        Emit("LOOT_COLLECTION_REFUSED historical-caller-not-observed; zero-added-hooks");
        return;
    }
    if (!Context->CheckExpectedBytes(CollectionHelperRva,
            ExpectedCollectionHelper.data(),
            static_cast<std::uint32_t>(ExpectedCollectionHelper.size()))) {
        Emit("LOOT_COLLECTION_REFUSED helper-fingerprint-differs-or-bridge-owned; zero-added-hooks");
        return;
    }
    // Publish the trampoline through the stable global exactly as the existing
    // item-code observer does, before the managed entry redirect goes live.
    if (!Context->InstallInlineHook(CollectionHelperRva,
            ExpectedCollectionHelper.data(),
            static_cast<std::uint32_t>(ExpectedCollectionHelper.size()),
            HookCollectionHelper, &OriginalCollectionHelper) ||
        !OriginalCollectionHelper) {
        Emit("LOOT_COLLECTION_REFUSED loader-hook-registration-failed; no-fallback-write");
        return;
    }
    CollectionHookInstalled.store(true, std::memory_order_release);
    Emit("LOOT_COLLECTION_OBSERVER_READY hook=D2R+0x1517C70 caller=D2R+0x1516ECC optional=1 readOnly=1 itemWrites=0 labelChanges=0");
}


// 0.2.19 EXPERIMENTAL, READ-ONLY: correlate the already hooked native item-
// code reader against explicitly user-marked world/hover/pickup phases.
// Neither the caller site nor a code-reader call proves sprite rendering or
// targeting; site RVAs are leads for the subsequent native renderer probe.
constexpr std::uint32_t WorldProbeExaltedCode =
    static_cast<std::uint32_t>('e') | (static_cast<std::uint32_t>('x')<<8U) |
    (static_cast<std::uint32_t>('o')<<16U);
constexpr std::size_t WorldProbeSiteLimit=32, WorldProbeTrackedLimit=128;
constexpr std::size_t WorldProbePhaseLimit=5;
// Each action is isolated: place test items at least three tiles apart;
// labels OFF and inventory closed throughout. The last phase is a control.
constexpr std::array<const char*,WorldProbePhaseLimit> WorldProbePhaseNames{{
    "isolated-hidden-hover", "isolated-hidden-click",
    "isolated-control-hover", "isolated-control-click",
    "both-in-view-mouse-away"}};
struct WorldProbeSite final {
    std::uintptr_t returnRva{};
    std::uint32_t code{},unitId{};
    std::uint64_t hits{},groundWitness{},otherGround{},badUnit{};
    std::uint64_t beforeClick{},nearClick{},afterClick{};
    ULONGLONG firstMs{},lastMs{};
    std::uint32_t thread{};
    std::array<std::uintptr_t,6> stack{};
    unsigned stackSize{};
};
// The C0420 unit-label formatter is downstream of some in-world unit
// selection. Its identity is a verified LABEL input, NOT the hit-test result,
// mouse target or world-model renderer. Observe it independently from the
// native code-reader and do not retain a borrowed unit pointer.
struct WorldProbeLabelUnit final {
    std::uint32_t code{},unitId{};
    std::uint64_t calls{},withText{},noText{},beforeClick{},nearClick{},afterClick{};
    std::uint64_t capturedStacks{},noKnownCallerStacks{},ambiguousCallerStacks{};
    ULONGLONG firstMs{},lastMs{};
};
// Per-item *sampled* synchronous stack provenance from the existing SoE
// callback or standalone identity hook. These are RETURN-ADDRESS witnesses,
// never a retained native unit pointer and never renderer / picker proof.
struct WorldProbeLabelCaller final {
    std::uint32_t code{},unitId{};
    std::uintptr_t returnRva{};
    std::uint64_t samples{},nearClick{};
    unsigned firstFrameIndex{};
};
constexpr std::size_t WorldProbeLabelCallerLimit=32;
std::array<WorldProbeLabelCaller,WorldProbeLabelCallerLimit> WorldProbeLabelCallers{};
std::size_t WorldProbeLabelCallerCount{};
struct WorldProbeStackExample final {
    std::uint32_t code{},unitId{};
    unsigned depth{};
    std::uint32_t matchedSite{},otherMatchedSites{};
    std::array<std::uintptr_t,18> frameRvas{};
};
constexpr std::size_t WorldProbeStackExampleLimit=4;
std::array<WorldProbeStackExample,WorldProbeStackExampleLimit> WorldProbeStackExamples{};
std::size_t WorldProbeStackExampleCount{};
std::atomic<std::uint32_t> WorldProbeLastStackCode{};
std::atomic<std::uint64_t> WorldProbeStackSamples{},WorldProbeStackUnmatched{},
    WorldProbeStackAmbiguous{},WorldProbeStackDropped{};
constexpr std::size_t WorldProbeLabelLimit=16;
std::array<WorldProbeLabelUnit,WorldProbeLabelLimit> WorldProbeLabelUnits{};
std::size_t WorldProbeLabelCount{};
std::atomic<std::uint64_t> WorldProbeLabelCalls{},WorldProbeLabelContention{},
    WorldProbeLabelOverflow{};
std::mutex WorldProbeMutex;
std::array<WorldProbeSite,WorldProbeSiteLimit> WorldProbeSites{};
std::size_t WorldProbeSiteCount{};
std::atomic<int> WorldProbePhase{-1};
std::atomic_bool WorldProbeRunning{};
std::atomic_bool WorldProbePollPending{};
std::atomic<std::uint64_t> WorldProbeReads{},WorldProbeReadFailures{},
    WorldProbeOverflow{},WorldProbeContended{},WorldProbeGroundSamples{},
    WorldProbeCarriedHidden{},WorldProbeCarriedControl{};
std::atomic<std::uint32_t> WorldProbeLastCarriedHidden{},WorldProbeLastCarriedControl{};
// Click samples are a FOREGROUND Win32 button-level observation from a worker,
// not a D2R event and not an item-target / pickup proof. Used ONLY by probe.
std::atomic<ULONGLONG> WorldProbeLastClickMs{};
std::atomic<std::uint32_t> WorldProbeClickEdges{},WorldProbeBaselinePasses{},
    WorldProbePostClickPasses{},WorldProbeMissedBaseline{},WorldProbeEpoch{};
std::atomic_bool WorldProbeBaselineReady{};
// Per-session identity token from the ground-label callback. The native model
// and picker must later be independently established.
std::array<std::atomic<std::uint64_t>,WorldProbeTrackedLimit> WorldProbeBaseline{};
std::array<std::atomic<std::uint64_t>,WorldProbeTrackedLimit> WorldProbeReported{};
std::atomic<std::uint32_t> WorldProbeProbeSampleMs{20};

// 0.2.26 automatic, click-bounded candidate provenance. This never
// intervenes in native mouse selection, item state, network pickup or UI.
// A prior in-world label identity is a HINT, not the engine's click target.
constexpr ULONGLONG PickupWindowMs=1500;
constexpr std::size_t PickupSitesLimit=48;
struct PickupNativeSite final {
    std::uintptr_t returnRva{};
    std::uint32_t code{},unitId{};
    std::uint64_t hits{};
    std::uint64_t sdkInventoryPollHits{},outsideSdkPollHits{};
    ULONGLONG firstMs{},lastMs{};
    bool recentGroundWitness{};
    // One bounded call stack for the clicked candidate at this exact reader
    // site. Capture addresses only; no borrowed native unit pointers.
    std::array<std::uintptr_t,12> firstStack{};
    unsigned firstStackDepth{},firstThreadId{};
    bool stackAttempted{};
};
// Marks ONLY our synchronous SDK inventory/cursor snapshot. The native
// code-reader hook may be called reentrantly on this exact UI thread; do not
// mistake those reads for calls from D2R's mouse picker.
thread_local bool PickupSdkInventoryPollActive=false;
struct PickupSdkInventoryPollScope final {
    bool previous;
    PickupSdkInventoryPollScope() noexcept
        :previous(PickupSdkInventoryPollActive){
        PickupSdkInventoryPollActive=true;
    }
    ~PickupSdkInventoryPollScope() noexcept {
        PickupSdkInventoryPollActive=previous;
    }
    PickupSdkInventoryPollScope(const PickupSdkInventoryPollScope&)=delete;
    PickupSdkInventoryPollScope& operator=(const PickupSdkInventoryPollScope&)=delete;
};
std::mutex PickupSitesMutex;
std::array<PickupNativeSite,PickupSitesLimit> PickupSites{};
std::size_t PickupSiteCount{};
std::atomic<ULONGLONG> PickupClickMs{};
std::atomic<std::uint64_t> PickupHintToken{},PickupHoverToken{},PickupPreToken{};
std::atomic<ULONGLONG> PickupHoverMs{},PickupPreMs{},PickupPostFirstCarriedMs{};
// Snapshot status: 0 unavailable, 1 present snapshot/absent, 2 carried.
std::atomic<unsigned> PickupPreStatus{},PickupPostStatus{},PickupBaselineAtClick{};
std::atomic_bool PickupPollPending{};
std::atomic<ULONGLONG> PickupPollScheduledMs{};
std::atomic<std::uint32_t> PickupClickEpoch{},PickupPrePasses{},PickupPostPasses{},PickupBaselinePassesAtClick{};
std::atomic<std::uint64_t> PickupNativeReadFailures{},PickupNativeContention{},
    PickupNativeOverflow{};
void HiddenPickupReset() noexcept {
    PickupClickMs.store(0,std::memory_order_release);
    PickupHintToken.store(0,std::memory_order_release);
    PickupHoverToken.store(0,std::memory_order_release);
    PickupHoverMs.store(0,std::memory_order_release);
    PickupPreToken.store(0,std::memory_order_release);
    PickupPreMs.store(0,std::memory_order_release);
    PickupPollScheduledMs.store(0,std::memory_order_release);
    PickupPreStatus.store(0,std::memory_order_release);
    PickupPostStatus.store(0,std::memory_order_release);
    PickupBaselineAtClick.store(0,std::memory_order_release);
    PickupPostFirstCarriedMs.store(0,std::memory_order_release);
    PickupClickEpoch.fetch_add(1,std::memory_order_acq_rel);
    PickupPrePasses.store(0,std::memory_order_release);
    PickupBaselinePassesAtClick.store(0,std::memory_order_release);
    PickupPostPasses.store(0,std::memory_order_release);
    PickupNativeReadFailures.store(0,std::memory_order_release);
    PickupNativeContention.store(0,std::memory_order_release);
    PickupNativeOverflow.store(0,std::memory_order_release);
    // In-flight UI polling reads only owned atomic values; do NOT force-clear
    // PickupPollPending because the scheduled callback may still be active.
    if(PickupSitesMutex.try_lock()){
        PickupSites.fill({});PickupSiteCount=0;PickupSitesMutex.unlock();
    }
}

// Token = (canonical code << 32) | runtime ID. No native pointers survive.
std::array<std::atomic<std::uint64_t>,WorldProbeTrackedLimit> WorldProbeGround{};
std::jthread WorldProbeWorker{};
bool WorldProbeMonitoredCode(std::uint32_t code) noexcept;

// 0.2.39: Build-93847 native action witness. Consumer and queue detours always
// forward. Dispatch only skips a qualified show:false ground-item action 22;
// all other actions forward. No target clearing or input swallowing.
namespace ActionTrace = NativeActionTracePolicy;
namespace PickupGuard = NativePickupGuardPolicy;
constexpr std::uintptr_t NativeActionConsumerRva=0xF9BC0;
constexpr std::uintptr_t NativeActionQueueRva=0xFBEF0;
constexpr std::uintptr_t NativeActionDispatchRva=0xFABE0;
constexpr std::uintptr_t NativeItemLookupRva=0x9A5D0;
constexpr std::uintptr_t NativeVerifiedPickupCallRva=0x101ADF;
using NativeItemLookupFn=void*(__fastcall*)(std::uint32_t,std::uint32_t) noexcept;
std::atomic_bool NativePickupGuardQualified{};
std::atomic<std::uint64_t> NativePickupGuardCandidates{},
    NativePickupGuardBlocked{},NativePickupGuardLookupFailed{},
    NativePickupGuardInvalidIdentity{},NativePickupGuardWrongMode{},
    NativePickupGuardNoRule{};
std::atomic<std::uint32_t> NativePickupGuardLastId{},
    NativePickupGuardLastCode{},NativePickupGuardLastMode{};
thread_local bool NativePickupGuardInside=false;
// Always-on and independent of the optional F10 native-action capture phase.
// Record a small immutable decision in the hook; write loader logs ONLY from
// the existing background worker. Never store native unit/player pointers.
struct NativePickupDecision final {
    PickupGuard::Decision reason{PickupGuard::Decision::GuardInactive};
    std::uint32_t code{};
    std::uint32_t mode{0xffffffffU};
    std::uint64_t generation{};
};
struct NativePickupDecisionEvent final {
    ULONGLONG timestamp{};
    std::uintptr_t callerRva{};
    std::uint64_t generation{};
    std::uint32_t itemId{},code{},mode{},threadId{};
    std::uint32_t armed{},geometry{},qualified{};
    PickupGuard::Decision reason{PickupGuard::Decision::GuardInactive};
    bool callerInD2R{},captureActive{};
};
constexpr std::size_t NativePickupDecisionCapacity=256;
std::mutex NativePickupDecisionMutex;
std::array<NativePickupDecisionEvent,NativePickupDecisionCapacity> NativePickupDecisions{};
std::size_t NativePickupDecisionCount{};
std::atomic<std::uint64_t> NativePickupDecisionSeen{},NativePickupDecisionForwarded{},
    NativePickupDecisionDropped{},NativePickupDecisionLogged{};

using NativeActionConsumerFn=void(__fastcall*)(void*) noexcept;
using NativeActionQueueFn=void(__fastcall*)(void*,std::uint32_t,
    std::uint32_t,std::uint32_t) noexcept;
using NativeActionDispatchFn=void(__fastcall*)(std::uint32_t,void*,
    std::uint32_t,std::uint32_t) noexcept;
NativeActionConsumerFn OriginalNativeActionConsumer{};
NativeActionQueueFn OriginalNativeActionQueue{};
NativeActionDispatchFn OriginalNativeActionDispatch{};
std::atomic_bool NativeActionConsumerInstalled{},NativeActionQueueInstalled{},
    NativeActionDispatchInstalled{};
constexpr std::size_t NativeActionMaxEvents=320;
struct NativeActionEvent final {
    std::uint64_t sequence{},token{};
    ULONGLONG timestamp{};
    std::uintptr_t callerRva{};
    std::uint32_t thread{},action{},targetType{},targetId{};
    std::uint32_t pendingAction{},pendingType{},pendingId{},pendingFlag{};
    unsigned phase{},site{},stage{},pendingReadable{},callerInD2R{};
};
std::mutex NativeActionMutex;
std::array<NativeActionEvent,NativeActionMaxEvents> NativeActionEvents{};
std::size_t NativeActionCount{};
std::atomic<std::uint64_t> NativeActionDropped{},NativeActionConsumerEmpty{},
    NativeActionConsumerPendingOther{},NativeActionDispatchOther{},
    NativeActionQueueOther{},NativeActionReadFailures{},NativeActionSequence{};
std::atomic<int> NativeActionPhase{-1};
std::atomic<unsigned> NativeActionEpoch{};
std::atomic<ULONGLONG> NativeActionStarted{},NativeActionDeadline{},
    NativeActionClickMs{};
std::atomic<unsigned> NativeActionClickCount{};
constexpr std::array<const char*,3> NativeActionPhaseNames{{
    "hidden-exalted", "visible-divine", "empty-ground"}};
constexpr std::array<const char*,3> NativeActionSites{{
    "F9BC0-consumer", "FBEF0-queue", "FABE0-dispatch"}};
constexpr std::array<const char*,2> NativeActionStages{{"entry", "return"}};

bool NativeActionReadPending(void* player,ActionTrace::Pending& pending) noexcept {
    if (!player) return false;
    // Native unit header type 0 is the player. The interface is read-only and
    // guarded for a stale, null or otherwise unreadable pointer.
    std::uint32_t unitType=0xffffffffU;
    SIZE_T count{};
    if (!ReadProcessMemory(GetCurrentProcess(),player,&unitType,
            sizeof(unitType),&count) || count!=sizeof(unitType) || unitType!=0)
        return false;
    void* state{};
    count=0;
    if (!ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const std::byte*>(player) +
                ActionTrace::PlayerInteractionDataOffset,
            &state,sizeof(state),&count) || count!=sizeof(state) || !state)
        return false;
    count=0;
    return ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const std::byte*>(state) +
            ActionTrace::PendingActionFieldsOffset,
        &pending,sizeof(pending),&count)!=0 && count==sizeof(pending);
}

void NativeActionObserve(unsigned site,unsigned stage,void* player,
    std::uint32_t action,std::uint32_t type,std::uint32_t id,
    std::uintptr_t caller) noexcept {
    const int phase=NativeActionPhase.load(std::memory_order_acquire);
    if (phase<0 || phase>=static_cast<int>(NativeActionPhaseNames.size()))return;
    const auto now=GetTickCount64();
    if (now>NativeActionDeadline.load(std::memory_order_acquire))return;
    ActionTrace::Pending pending{};
    const bool readable=NativeActionReadPending(player,pending);
    if (!readable)NativeActionReadFailures.fetch_add(1,std::memory_order_relaxed);
    if(site==0) {
        // F9BC0 processes queued actions, generally every frame. Retain the
        // entry identity on return so we still observe when it was cleared.
        if(stage==0) {
            if(!readable || pending.flag==0){
                NativeActionConsumerEmpty.fetch_add(1,std::memory_order_relaxed);
                return;
            }
            if(!ActionTrace::IsPendingItem(pending)){
                NativeActionConsumerPendingOther.fetch_add(1,
                    std::memory_order_relaxed);return;
            }
            action=pending.action;type=pending.targetType;id=pending.targetId;
        } else if(!ActionTrace::IsItemInteraction(type,id))return;
    } else if (!ActionTrace::IsItemInteraction(type,id)) {
        (site==1?NativeActionQueueOther:NativeActionDispatchOther)
            .fetch_add(1,std::memory_order_relaxed);
        return;
    }
    if(!NativeActionMutex.try_lock()){
        NativeActionDropped.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    if(NativeActionPhase.load(std::memory_order_acquire)!=phase ||
        now>NativeActionDeadline.load(std::memory_order_relaxed)){
        NativeActionMutex.unlock();return;
    }
    if(NativeActionCount>=NativeActionMaxEvents){
        NativeActionDropped.fetch_add(1,std::memory_order_relaxed);
        NativeActionMutex.unlock();return;
    }
    const auto witness=WorldProbeGround[id%WorldProbeTrackedLimit].load(
        std::memory_order_acquire);
    auto& e=NativeActionEvents[NativeActionCount++];
    e={};
    e.sequence=NativeActionSequence.fetch_add(1,std::memory_order_relaxed)+1;
    e.timestamp=now;
    e.thread=GetCurrentThreadId();
    e.action=action;e.targetType=type;e.targetId=id;
    e.pendingAction=pending.action;
    e.pendingType=pending.targetType;e.pendingId=pending.targetId;
    e.pendingFlag=pending.flag;e.pendingReadable=readable?1U:0U;
    e.phase=static_cast<unsigned>(phase);e.site=site;e.stage=stage;
    e.callerInD2R=(caller>=Base && caller-Base<ImageSize)?1U:0U;
    e.callerRva=e.callerInD2R?caller-Base:0;
    // Only annotate code where a previous label/painter observation explicitly
    // linked the same native unit ID to exo/divo. NOT fresh target proof.
    if(static_cast<std::uint32_t>(witness)==id &&
       WorldProbeMonitoredCode(static_cast<std::uint32_t>(witness>>32U)))
        e.token=witness;
    NativeActionMutex.unlock();
}

void __fastcall HookNativeActionConsumer(void* player) noexcept {
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    ActionTrace::Pending entry{};
    const bool observing=NativeActionPhase.load(std::memory_order_acquire)>=0;
    if(observing){
        (void)NativeActionReadPending(player,entry);
        NativeActionObserve(0,0,player,0,0,0,caller);
    }
    OriginalNativeActionConsumer(player);
    if(observing && ActionTrace::IsPendingItem(entry))
        NativeActionObserve(0,1,player,entry.action,
            entry.targetType,entry.targetId,caller);
}
void __fastcall HookNativeActionQueue(void* player,std::uint32_t action,
    std::uint32_t type,std::uint32_t id) noexcept {
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    NativeActionObserve(1,0,player,action,type,id,caller);
    OriginalNativeActionQueue(player,action,type,id);
    NativeActionObserve(1,1,player,action,type,id,caller);
}
// 0.2.39 build-93847: an action-22/type-4 candidate is independent of
// caller provenance. Both direct and deferred native action dispatches use
// fresh type/id lookup, current ground mode, and current show:false rules.
// The caller RVA is retained for diagnostics only; unknown callers never
// authorize a block without all of the same identity/rule checks. No native
// unit, item, target-slot, input, or player memory is written.
NativePickupDecision QualifyGroundPickup(std::uint32_t action,void* player,
    std::uint32_t type,std::uint32_t id) noexcept {
    NativePickupDecision result{};
    const auto fail=[&result](PickupGuard::Decision reason) noexcept {
        result.reason=reason;
        return result;
    };
    if(!NativePickupGuardQualified.load(std::memory_order_acquire))
        return fail(PickupGuard::Decision::GuardInactive);
    if(id==0) return fail(PickupGuard::Decision::InvalidTargetId);
    // Caller origin is NOT a filter authorization criterion. The observed
    // D2R+0x101AE4 immediate and D2R+0xFA11A deferred paths must share the
    // identical fresh item/mode/rule qualification; caller is logged below.
    if(!PickupGuard::Candidate(action,type,id))
        return fail(PickupGuard::Decision::InvalidTargetId);
    if(!HideGroundArmed.load(std::memory_order_acquire))
        return fail(PickupGuard::Decision::VisibilityNotArmed);
    if(ActiveGeometryMode.load(std::memory_order_acquire)!=GeometryMode::Rules)
        return fail(PickupGuard::Decision::RulesModeInactive);
    if(!HookInstalled.load(std::memory_order_acquire) || !OriginalGetItemCode)
        return fail(PickupGuard::Decision::CodeReaderUnavailable);
    if(!player) return fail(PickupGuard::Decision::NullPlayer);
    if(NativePickupGuardInside) return fail(PickupGuard::Decision::Reentrant);
    const auto rules=std::atomic_load_explicit(&PublishedFilterRules,
        std::memory_order_acquire);
    if(!rules || !rules->hiddenRules)
        return fail(PickupGuard::Decision::NoHiddenRules);
    result.generation=rules->generation;
    NativePickupGuardCandidates.fetch_add(1,std::memory_order_relaxed);
    std::uint32_t playerType=0xffffffff;
    SIZE_T copied{};
    if(!ReadProcessMemory(GetCurrentProcess(),player,&playerType,
            sizeof(playerType),&copied) || copied!=sizeof(playerType) ||
       playerType!=0) {
        NativePickupGuardInvalidIdentity.fetch_add(1,std::memory_order_relaxed);
        return fail(PickupGuard::Decision::InvalidPlayer);
    }
    // Direct lookup admitted only after exact callsite/entry checks in
    // NativeActionInstall. No cached label hint authorizes a block.
    NativePickupGuardInside=true;
    auto* unit=reinterpret_cast<NativeItemLookupFn>(Base+NativeItemLookupRva)(
        id,PickupGuard::ItemUnitType);
    NativePickupGuardInside=false;
    if(!unit){
        NativePickupGuardLookupFailed.fetch_add(1,std::memory_order_relaxed);
        return fail(PickupGuard::Decision::LookupFailed);
    }
    std::array<std::uint32_t,4> header{};
    copied=0;
    if(!ReadProcessMemory(GetCurrentProcess(),unit,header.data(),
            sizeof(header),&copied) || copied!=sizeof(header) ||
       !PickupGuard::SameItemIdentity(header[0],header[2],id)){
        NativePickupGuardInvalidIdentity.fetch_add(1,std::memory_order_relaxed);
        return fail(PickupGuard::Decision::InvalidUnit);
    }
    result.mode=header[3];
    NativePickupGuardLastMode.store(header[3],std::memory_order_relaxed);
    // UnitAny.mode DWORD at +0xC; do not broaden mode 3 speculatively.
    if(!PickupGuard::GroundMode(header[3])){
        NativePickupGuardWrongMode.fetch_add(1,std::memory_order_relaxed);
        return fail(PickupGuard::Decision::NotGround);
    }
    NativePickupGuardInside=true;
    result.code=CanonicalItemCode(OriginalGetItemCode(unit));
    NativePickupGuardInside=false;
    if(!PrintableItemCode(result.code)){
        NativePickupGuardInvalidIdentity.fetch_add(1,std::memory_order_relaxed);
        return fail(PickupGuard::Decision::InvalidCode);
    }
    const auto ruleItem=GroundRuleItem(result.code,unit,rules.get(),id);
    GroundRuleDecision resolvedRule{};
    const auto* matchedRule=ResolveGroundRule(rules.get(),ruleItem,resolvedRule) ?
        &resolvedRule : nullptr;
    if(!matchedRule || matchedRule->show){
        NativePickupGuardNoRule.fetch_add(1,std::memory_order_relaxed);
        return fail(PickupGuard::Decision::NoHiddenRule);
    }
    NativePickupGuardLastId.store(id,std::memory_order_relaxed);
    NativePickupGuardLastCode.store(result.code,std::memory_order_relaxed);
    NativePickupGuardBlocked.fetch_add(1,std::memory_order_relaxed);
    // FUN_1400fabe0 returns void; neither direct nor deferred caller consumes a result.
    result.reason=PickupGuard::Decision::Blocked;
    return result;
}

void RecordPickupDecision(std::uint32_t id,std::uintptr_t caller,
    const NativePickupDecision& result) noexcept {
    NativePickupDecisionSeen.fetch_add(1,std::memory_order_relaxed);
    if(result.reason!=PickupGuard::Decision::Blocked)
        NativePickupDecisionForwarded.fetch_add(1,std::memory_order_relaxed);
    if(!NativePickupDecisionMutex.try_lock()){
        NativePickupDecisionDropped.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    if(NativePickupDecisionCount>=NativePickupDecisionCapacity){
        NativePickupDecisionDropped.fetch_add(1,std::memory_order_relaxed);
        NativePickupDecisionMutex.unlock();return;
    }
    auto& e=NativePickupDecisions[NativePickupDecisionCount++];
    e={};
    e.timestamp=GetTickCount64();
    e.itemId=id;e.code=result.code;e.mode=result.mode;
    e.generation=result.generation;
    e.reason=result.reason;
    e.threadId=GetCurrentThreadId();
    e.callerInD2R=caller>=Base && caller-Base<ImageSize;
    e.callerRva=e.callerInD2R?caller-Base:0;
    e.armed=HideGroundArmed.load(std::memory_order_relaxed)?1U:0U;
    e.geometry=static_cast<unsigned>(ActiveGeometryMode.load(
        std::memory_order_relaxed));
    e.qualified=NativePickupGuardQualified.load(std::memory_order_relaxed)?1U:0U;
    e.captureActive=NativeActionPhase.load(std::memory_order_relaxed)>=0;
    NativePickupDecisionMutex.unlock();
}

// Worker-only drain. No window, phase, hotkey, or seven-second timer gates it.
void DrainPickupDecisions() noexcept {
    std::array<NativePickupDecisionEvent,NativePickupDecisionCapacity> rows{};
    std::size_t count{};
    {
        std::lock_guard lock(NativePickupDecisionMutex);
        count=NativePickupDecisionCount;
        if(count) std::copy_n(NativePickupDecisions.begin(),count,rows.begin());
        NativePickupDecisionCount=0;
    }
    for(std::size_t i=0;i<count;++i){
        const auto& e=rows[i];
        char code[5]{};
        CodeText(e.code,code);
        char line[470]{};
        std::snprintf(line,sizeof(line),
            "LOOT_PICKUP_GUARD_DECISION version=1.0.0 ms=%llu "
            "outcome=%s reason=%s action=22 targetType=4 targetId=%u "
            "code='%.4s' unitMode=%u callerRva=D2R+0x%llX "
            "callerInD2R=%u rulesGeneration=%llu qualified=%u "
            "visibilityArmed=%u geometryMode=%u captureActive=%u "
            "tid=%u phaseIndependent=1",
            static_cast<unsigned long long>(e.timestamp),
            e.reason==PickupGuard::Decision::Blocked?"BLOCKED":"FORWARDED",
            PickupGuard::DecisionName(e.reason),e.itemId,code,e.mode,
            static_cast<unsigned long long>(e.callerRva),
            e.callerInD2R?1U:0U,
            static_cast<unsigned long long>(e.generation),e.qualified,e.armed,
            e.geometry,e.captureActive?1U:0U,e.threadId);
        Emit(line);
    }
    NativePickupDecisionLogged.fetch_add(count,std::memory_order_relaxed);
}

void __fastcall HookNativeActionDispatch(std::uint32_t action,void* player,
    std::uint32_t type,std::uint32_t id) noexcept {
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    NativeActionObserve(2,0,player,action,type,id,caller);
    // Never call a native helper for other action codes/unit types. Logging is
    // active for EVERY observed action-22 item dispatch, regardless of F10.
    if(action==PickupGuard::PickupAction && type==PickupGuard::ItemUnitType){
        const auto decision=QualifyGroundPickup(action,player,type,id);
        RecordPickupDecision(id,caller,decision);
        if(decision.reason==PickupGuard::Decision::Blocked)return;
    }
    OriginalNativeActionDispatch(action,player,type,id);
    NativeActionObserve(2,1,player,action,type,id,caller);
}

bool NativeActionCallMatches(std::uintptr_t callRva,
    std::uintptr_t targetRva) noexcept {
    std::array<std::uint8_t,5> bytes{};
    if(!ReadSafe(callRva,bytes.data(),bytes.size()) || bytes[0]!=0xe8)
        return false;
    std::int32_t relative{};
    std::memcpy(&relative,bytes.data()+1,sizeof(relative));
    return static_cast<std::int64_t>(callRva)+5+relative==
        static_cast<std::int64_t>(targetRva);
}
void NativeActionInstall() noexcept {
    if(!Context || !Base || !ImageSize ||
       !D2RL::GetBuildName(Context) ||
       std::string_view(D2RL::GetBuildName(Context))!="93847"){
        Emit("LOOT_NATIVE_ACTION_REFUSED reason=wrong-build-or-image zero-new-hooks=1");
        return;
    }
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!ReadSafe(0,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
       dos.e_lfanew<=0 || dos.e_lfanew>0x1000 ||
       !ReadSafe(static_cast<std::uintptr_t>(dos.e_lfanew),&nt,sizeof(nt)) ||
       nt.Signature!=IMAGE_NT_SIGNATURE ||
       nt.FileHeader.TimeDateStamp!=0x6AB3782C ||
       nt.OptionalHeader.SizeOfImage!=ImageSize){
        Emit("LOOT_NATIVE_ACTION_REFUSED reason=PE-timestamp-or-image-size-mismatch zero-new-hooks=1");
        return;
    }
    // All six observed E8 edges come from the user's raw-image Ghidra report.
    // Validate their live targets, rather than assuming a decompilation proves
    // an RVAs' entry is still unowned after other plugins are loaded.
    constexpr std::array<std::pair<std::uintptr_t,std::uintptr_t>,8> edges{{
        {0x8BF71,NativeActionConsumerRva},
        {0x95041,NativeActionConsumerRva},
        {0xF9ED5,NativeActionQueueRva},
        {0xF9F35,NativeActionQueueRva},
        {0xFA094,NativeActionQueueRva},
        {0xFA103,NativeActionQueueRva},
        {0xFA115,NativeActionDispatchRva},
        {0xFBF30,NativeActionDispatchRva}
    }};
    for(const auto& edge:edges){
        if(!NativeActionCallMatches(edge.first,edge.second)){
            char line[230]{};
            std::snprintf(line,sizeof(line),
                "LOOT_NATIVE_ACTION_REFUSED reason=callsite-witness-mismatch site=D2R+0x%llX zero-new-hooks=1",
                static_cast<unsigned long long>(edge.first));
            Emit(line);return;
        }
    }
    // Keep the observed direct call edge as a BUILD/ABI integrity witness,
    // NOT a requirement on the live caller. The deferred path D2R+0xFA115
    // -> FABE0 is also independently checked in edges above. Qualify the
    // native unit-by-id helper used by FABE0 itself. No global event veto.
    constexpr std::array<std::uint8_t,3> lookupEntry{{0x4C,0x63,0xCA}};
    const bool lookupQualified=
        NativeActionCallMatches(NativeVerifiedPickupCallRva,
            NativeActionDispatchRva) &&
        NativeActionCallMatches(0xFACB9,NativeItemLookupRva) &&
        Context->CheckExpectedBytes(NativeItemLookupRva,
            lookupEntry.data(),static_cast<std::uint32_t>(lookupEntry.size()));
    if(!lookupQualified)
        Emit("LOOT_PICKUP_GUARD_REFUSED reason=direct-call-edge-or-lookup-witness-mismatch forward-only=1");
    // F9BC0 is recorded instruction-for-instruction; additional entry checks
    // for FBEF0/FABE0 use an exact snapshot of LIVE bytes after validating the
    // PE, call graph and absence of existing JMP/FF25 bridge. This is NOT an
    // independent static fingerprint. Loader still rejects changed bytes.
    constexpr std::array<std::uint8_t,18> consumerEntry{{
        0x48,0x85,0xC9,0x0F,0x84,0x78,0x05,0x00,0x00,
        0x55,0x53,0x48,0x8B,0xEC,0x48,0x83,0xEC,0x68}};
    if(!Context->CheckExpectedBytes(NativeActionConsumerRva,
            consumerEntry.data(),static_cast<std::uint32_t>(consumerEntry.size()))){
        Emit("LOOT_NATIVE_ACTION_REFUSED reason=consumer-exact-entry-mismatch zero-new-hooks=1");
        return;
    }
    std::array<std::uint8_t,20> queueEntry{},dispatchEntry{};
    if(!ReadSafe(NativeActionQueueRva,queueEntry.data(),queueEntry.size()) ||
       !ReadSafe(NativeActionDispatchRva,dispatchEntry.data(),dispatchEntry.size()) ||
       !((queueEntry[0]==0x40)||(queueEntry[0]==0x48)||
         (queueEntry[0]>=0x50 && queueEntry[0]<=0x57)) ||
       !((dispatchEntry[0]==0x40)||(dispatchEntry[0]==0x48)||
         (dispatchEntry[0]>=0x50 && dispatchEntry[0]<=0x57)) ||
       (queueEntry[0]==0x48 && queueEntry[1]==0xFF && queueEntry[2]==0x25) ||
       (dispatchEntry[0]==0x48 && dispatchEntry[1]==0xFF && dispatchEntry[2]==0x25) ||
       !Context->CheckExpectedBytes(NativeActionQueueRva,
            queueEntry.data(),static_cast<std::uint32_t>(queueEntry.size())) ||
       !Context->CheckExpectedBytes(NativeActionDispatchRva,
            dispatchEntry.data(),static_cast<std::uint32_t>(dispatchEntry.size()))){
        Emit("LOOT_NATIVE_ACTION_REFUSED reason=queue-or-dispatch-entry-unqualified zero-new-hooks=1");
        return;
    }
    // Install deepest callee first; dispatch blocks only qualified hidden
    // ground-item action 22, while queue and consumer always forward.
    if(!Context->InstallInlineHook(NativeActionDispatchRva,
            dispatchEntry.data(),static_cast<std::uint32_t>(dispatchEntry.size()),
            HookNativeActionDispatch,&OriginalNativeActionDispatch) ||
       !OriginalNativeActionDispatch){
        Emit("LOOT_NATIVE_ACTION_REFUSED reason=dispatch-loader-hook-failed no-probe-armed=1");
        return;
    }
    NativeActionDispatchInstalled.store(true,std::memory_order_release);
    if(!Context->InstallInlineHook(NativeActionQueueRva,
            queueEntry.data(),static_cast<std::uint32_t>(queueEntry.size()),
            HookNativeActionQueue,&OriginalNativeActionQueue) ||
       !OriginalNativeActionQueue){
        Emit("LOOT_NATIVE_ACTION_PARTIAL reason=queue-loader-hook-failed no-probe-armed=1 dispatch-forward-only=1");
        return;
    }
    NativeActionQueueInstalled.store(true,std::memory_order_release);
    if(!Context->InstallInlineHook(NativeActionConsumerRva,
            consumerEntry.data(),static_cast<std::uint32_t>(consumerEntry.size()),
            HookNativeActionConsumer,&OriginalNativeActionConsumer) ||
       !OriginalNativeActionConsumer){
        Emit("LOOT_NATIVE_ACTION_PARTIAL reason=consumer-loader-hook-failed no-probe-armed=1 other-hooks-forward-only=1");
        return;
    }
    NativeActionConsumerInstalled.store(true,std::memory_order_release);
    NativePickupGuardQualified.store(lookupQualified,std::memory_order_release);
    Emit(lookupQualified ?
        "LOOT_PICKUP_GUARD_READY version=1.0.0 action=22 type=4 callerPolicy=none observedImmediate=D2R+0x101AE4 observedDeferred=D2R+0xFA11A mode=3 freshLookup=D2R+0x9A5D0 codeSource=qualified-original-helper rule=show:false stateWrites=0 returnContract=void decisionLog=always-on captureKey=optional" :
        "LOOT_PICKUP_GUARD_INACTIVE version=1.0.0 reason=lookup-or-caller-witness forward-only=1");
    Emit("LOOT_NATIVE_ACTION_READY version=1.0.0 build=93847 "
        "hooks=F9BC0,FBEF0,FABE0 forwardOriginal=1 "
        "qualifier=PE-timestamp+eight-exact-E8-edges+entry-guards "
        "F9BC0-entry=static-exact FBEF0/FABE0-entry=live-snapshot-not-static-fingerprint "
        "capture=Ctrl+Shift+F10 click-observation=Win32 edge "
        "gameplayWrites=0 slotWrites=0 clickSuppression=qualified-show-false-only");
}

void NativeActionStartPhase(ULONGLONG now) noexcept {
    if(!NativeActionConsumerInstalled.load(std::memory_order_acquire) ||
       !NativeActionQueueInstalled.load(std::memory_order_acquire) ||
       !NativeActionDispatchInstalled.load(std::memory_order_acquire)){
        Emit("LOOT_NATIVE_ACTION_UNAVAILABLE reason=not-all-three-hooks-qualified");
        return;
    }
    if(NativeActionPhase.load(std::memory_order_acquire)>=0){
        Emit("LOOT_NATIVE_ACTION_BUSY wait-for-current-seven-second-phase");
        return;
    }
    const unsigned phase=NativeActionEpoch.fetch_add(1,
        std::memory_order_acq_rel)%static_cast<unsigned>(NativeActionPhaseNames.size());
    NativeActionPhase.store(-1,std::memory_order_release);
    {
        std::lock_guard lock(NativeActionMutex);
        NativeActionCount=0;
        NativeActionEvents.fill({});
    }
    NativeActionDropped.store(0);NativeActionConsumerEmpty.store(0);
    NativeActionConsumerPendingOther.store(0);
    NativeActionQueueOther.store(0);NativeActionDispatchOther.store(0);
    NativeActionReadFailures.store(0);
    NativeActionClickMs.store(0,std::memory_order_release);
    NativeActionClickCount.store(0,std::memory_order_release);
    NativeActionStarted.store(now,std::memory_order_release);
    NativeActionDeadline.store(now+ActionTrace::PhaseDurationMs,
        std::memory_order_release);
    NativeActionPhase.store(static_cast<int>(phase),std::memory_order_release);
    char msg[360]{};
    std::snprintf(msg,sizeof(msg),
        "LOOT_NATIVE_ACTION_PHASE_BEGIN version=1.0.0 phase=%s durationMs=%llu "
        "trigger=Ctrl+Shift+F10 instruction=perform-one-isolated-click "
        "noItemOrTargetWrites=1 pickupGuard=qualified-show-false-only",
        NativeActionPhaseNames[phase],
        static_cast<unsigned long long>(ActionTrace::PhaseDurationMs));
    Emit(msg);
}
void NativeActionFinishPhase(ULONGLONG now) noexcept {
    int phase=NativeActionPhase.load(std::memory_order_acquire);
    if(phase<0 || now<NativeActionDeadline.load(std::memory_order_acquire))return;
    if(!NativeActionPhase.compare_exchange_strong(phase,-1,
            std::memory_order_acq_rel))return;
    // Publish no active phase BEFORE waiting for any in-flight hook recorder.
    std::array<NativeActionEvent,NativeActionMaxEvents> rows{};
    std::size_t count{};
    {
        std::lock_guard lock(NativeActionMutex);
        count=NativeActionCount;
        for(std::size_t i=0;i<count;++i)rows[i]=NativeActionEvents[i];
    }
    const auto click=NativeActionClickMs.load(std::memory_order_acquire);
    const auto totalClicks=NativeActionClickCount.load(std::memory_order_acquire);
    char line[610]{};
    std::snprintf(line,sizeof(line),
        "LOOT_NATIVE_ACTION_PHASE_END version=1.0.0 phase=%s events=%zu "
        "clickEdges=%u lastClickMs=%llu consumerNoPending=%llu "
        "consumerNonItemPending=%llu queueNonItem=%llu dispatchNonItem=%llu "
        "pendingReadFailure=%llu eventOverflowOrContention=%llu "
        "meaning=bounded-native-interaction-correlation-with-guard",
        NativeActionPhaseNames[static_cast<unsigned>(phase)],count,totalClicks,
        static_cast<unsigned long long>(click),
        static_cast<unsigned long long>(NativeActionConsumerEmpty.load()),
        static_cast<unsigned long long>(NativeActionConsumerPendingOther.load()),
        static_cast<unsigned long long>(NativeActionQueueOther.load()),
        static_cast<unsigned long long>(NativeActionDispatchOther.load()),
        static_cast<unsigned long long>(NativeActionReadFailures.load()),
        static_cast<unsigned long long>(NativeActionDropped.load()));
    Emit(line);
    {
        char guard[260]{};
        std::snprintf(guard,sizeof(guard),
            "LOOT_PICKUP_GUARD_PHASE version=1.0.0 phase=%s candidates=%llu blocked=%llu "
            "lastBlockedId=%u lastMode=%u",
            NativeActionPhaseNames[static_cast<unsigned>(phase)],
            static_cast<unsigned long long>(NativePickupGuardCandidates.load()),
            static_cast<unsigned long long>(NativePickupGuardBlocked.load()),
            NativePickupGuardLastId.load(),NativePickupGuardLastMode.load());
        Emit(guard);
    }
    for(std::size_t i=0;i<count;++i){
        const auto& e=rows[i];char code[5]{};
        CodeText(static_cast<std::uint32_t>(e.token>>32U),code);
        std::snprintf(line,sizeof(line),
            "LOOT_NATIVE_ACTION_EVENT phase=%s seq=%llu site=%s stage=%s "
            "ms=%llu clickDeltaMs=%lld nearClick=%u "
            "action=%u targetType=%u targetId=%u "
            "groundCodeHint='%.4s' hintMatchesTargetId=%u "
            "pendingReadable=%u pendingFlag=%u pendingAction=%u "
            "pendingType=%u pendingId=%u callerRva=D2R+0x%llX "
            "callerInD2R=%u tid=%u recorderWrites=0",
            NativeActionPhaseNames[e.phase],
            static_cast<unsigned long long>(e.sequence),
            NativeActionSites[e.site],NativeActionStages[e.stage],
            static_cast<unsigned long long>(e.timestamp),
            click?static_cast<long long>(e.timestamp)-
                static_cast<long long>(click):0LL,
            ActionTrace::NearClick(e.timestamp,click)?1U:0U,
            e.action,e.targetType,e.targetId,code,e.token?1U:0U,
            e.pendingReadable,e.pendingFlag,e.pendingAction,e.pendingType,
            e.pendingId,static_cast<unsigned long long>(e.callerRva),
            e.callerInD2R,e.thread);
        Emit(line);
    }
}

bool WorldProbeMonitoredCode(std::uint32_t code) noexcept {
    return code == DivineCode || code == WorldProbeExaltedCode;
}
std::uint64_t WorldProbeToken(std::uint32_t code,std::uint32_t id) noexcept {
    return (static_cast<std::uint64_t>(code)<<32U) | id;
}
void WorldProbeResetGround() noexcept {
    HiddenPickupReset();
    WorldProbePhase.store(-1,std::memory_order_release);
    for (auto& slot: WorldProbeGround)slot.store(0,std::memory_order_release);
    for (auto& slot: WorldProbeBaseline)slot.store(0,std::memory_order_release);
    for (auto& slot: WorldProbeReported)slot.store(0,std::memory_order_release);
    WorldProbeLastClickMs.store(0);
    WorldProbeClickEdges.store(0);
    WorldProbeBaselineReady.store(false);
    WorldProbeCarriedHidden.store(0,std::memory_order_release);
    WorldProbeCarriedControl.store(0,std::memory_order_release);
    WorldProbeLastCarriedHidden.store(0,std::memory_order_release);
    WorldProbeLastCarriedControl.store(0,std::memory_order_release);
}
void WorldProbeObserveGround(std::uint32_t code,std::uint32_t id) noexcept {
    if (!WorldProbeRunning.load(std::memory_order_relaxed) || !id ||
        !WorldProbeMonitoredCode(code)) return;
    WorldProbeGround[id%WorldProbeTrackedLimit].store(WorldProbeToken(code,id),
        std::memory_order_release);
    PickupHoverToken.store(WorldProbeToken(code,id),std::memory_order_release);
    PickupHoverMs.store(GetTickCount64(),std::memory_order_release);
    WorldProbeGroundSamples.fetch_add(1,std::memory_order_relaxed);
}
void WorldProbeObserveSelectedLabel(std::uint32_t code,std::uint32_t id,
                                    bool textAvailable) noexcept {
    const auto phase=WorldProbePhase.load(std::memory_order_acquire);
    if(phase<0 || !WorldProbeRunning.load(std::memory_order_relaxed) ||
       !id || !WorldProbeMonitoredCode(code))return;
    const auto epoch=WorldProbeEpoch.load(std::memory_order_acquire);
    const auto sequence=WorldProbeLabelCalls.fetch_add(1,std::memory_order_relaxed)+1;
    const bool differentCode=WorldProbeLastStackCode.exchange(code,
        std::memory_order_acq_rel)!=code;
    // A full Win64 unwind on EVERY hover callback would needlessly burden
    // the render/UI thread. Capture early samples, changes of item code,
    // and then one per 16 eligible callbacks. Captures are NOT exact counts.
    const bool capture=sequence<=16 || (sequence%16)==0 || differentCode;
    std::array<PVOID,32> frames{};
    unsigned frameCount{};
    WorldProbeLabelStack::Witness witness{};
    if(capture){
        frameCount=CaptureStackBackTrace(1,
            static_cast<DWORD>(frames.size()),frames.data(),nullptr);
        witness=WorldProbeLabelStack::Identify(
            std::span<const PVOID>(frames.data(),frameCount),Base,ImageSize);
    }
    if(!WorldProbeMutex.try_lock()){
        WorldProbeLabelContention.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    // Never attribute a stack unwind to the next user-marked phase.
    if(phase!=WorldProbePhase.load(std::memory_order_acquire) ||
       epoch!=WorldProbeEpoch.load(std::memory_order_acquire)){
        WorldProbeMutex.unlock();return;
    }
    WorldProbeLabelUnit* row=nullptr;
    for(std::size_t i=0;i<WorldProbeLabelCount;++i)
        if(WorldProbeLabelUnits[i].code==code &&
           WorldProbeLabelUnits[i].unitId==id){row=&WorldProbeLabelUnits[i];break;}
    if(!row && WorldProbeLabelCount<WorldProbeLabelUnits.size()){
        row=&WorldProbeLabelUnits[WorldProbeLabelCount++];
        *row={};row->code=code;row->unitId=id;
    }
    if(row){
        const auto now=GetTickCount64();
        const auto click=WorldProbeLastClickMs.load(std::memory_order_acquire);
        ++row->calls;
        if(textAvailable)++row->withText;else ++row->noText;
        if(!row->firstMs)row->firstMs=now;
        row->lastMs=now;
        if(!click || now<click)++row->beforeClick;
        else if(WorldProbeEvidence::NearObservedClick(now,click))++row->nearClick;
        else ++row->afterClick;
        if(capture){
            ++row->capturedStacks;
            WorldProbeStackSamples.fetch_add(1,std::memory_order_relaxed);
            if(witness.distinctSiteCount==0){
                ++row->noKnownCallerStacks;
                WorldProbeStackUnmatched.fetch_add(1,std::memory_order_relaxed);
            }else if(witness.distinctSiteCount>1){
                ++row->ambiguousCallerStacks;
                WorldProbeStackAmbiguous.fetch_add(1,std::memory_order_relaxed);
            }else{
                WorldProbeLabelCaller* caller=nullptr;
                for(std::size_t i=0;i<WorldProbeLabelCallerCount;++i)
                    if(WorldProbeLabelCallers[i].code==code &&
                       WorldProbeLabelCallers[i].unitId==id &&
                       WorldProbeLabelCallers[i].returnRva==witness.returnRva){
                        caller=&WorldProbeLabelCallers[i];break;
                    }
                if(!caller && WorldProbeLabelCallerCount<WorldProbeLabelCallers.size()){
                    caller=&WorldProbeLabelCallers[WorldProbeLabelCallerCount++];
                    *caller={};caller->code=code;caller->unitId=id;
                    caller->returnRva=witness.returnRva;
                    caller->firstFrameIndex=witness.frameIndex;
                }
                if(caller){
                    ++caller->samples;
                    if(click && now>=click && WorldProbeEvidence::NearObservedClick(now,click))
                        ++caller->nearClick;
                }else WorldProbeStackDropped.fetch_add(1,std::memory_order_relaxed);
            }
            // Keep up to two examples per code per phase. Record ONLY D2R
            // module-relative return RVAs, not foreign library addresses or
            // native/SoE pointers. This helps diagnose truncated unwinds.
            std::size_t existing{};
            for(std::size_t i=0;i<WorldProbeStackExampleCount;++i)
                if(WorldProbeStackExamples[i].code==code)++existing;
            if(existing<2 && WorldProbeStackExampleCount<WorldProbeStackExamples.size()){
                auto& example=WorldProbeStackExamples[WorldProbeStackExampleCount++];
                example={};example.code=code;example.unitId=id;
                example.depth=frameCount;
                example.matchedSite=static_cast<std::uint32_t>(witness.returnRva);
                example.otherMatchedSites=witness.distinctSiteCount>0?
                    witness.distinctSiteCount-1:0;
                for(std::size_t i=0;i<example.frameRvas.size() && i<frameCount;++i){
                    const auto address=reinterpret_cast<std::uintptr_t>(frames[i]);
                    example.frameRvas[i]=address>=Base && address-Base<ImageSize?
                        address-Base:0;
                }
            }
        }
    }else WorldProbeLabelOverflow.fetch_add(1,std::memory_order_relaxed);
    WorldProbeMutex.unlock();
}
void WorldProbeObserveItem(void* item,std::uint32_t rawCode,
                           std::uintptr_t caller) noexcept {
    if (WorldProbePhase.load(std::memory_order_relaxed)<0 || !item ||
        !WorldProbeMonitoredCode(CanonicalItemCode(rawCode)) ||
        caller<Base || caller>=Base+ImageSize) return;
    const auto code=CanonicalItemCode(rawCode);
    WorldProbeReads.fetch_add(1,std::memory_order_relaxed);
    std::array<std::uint32_t,3> hdr{};
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(),item,hdr.data(),
            sizeof(hdr),&copied) || copied!=sizeof(hdr) ||
        hdr[0]!=4 || !hdr[2]) {
        WorldProbeReadFailures.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    if (!WorldProbeMutex.try_lock()) {
        WorldProbeContended.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    const auto rva=caller-Base;
    WorldProbeSite* row=nullptr;
    for (std::size_t n=0;n<WorldProbeSiteCount;++n)
        if (WorldProbeSites[n].returnRva==rva &&
            WorldProbeSites[n].code==code &&
            WorldProbeSites[n].unitId==hdr[2]){row=&WorldProbeSites[n];break;}
    if (!row && WorldProbeSiteCount<WorldProbeSites.size()) {
        row=&WorldProbeSites[WorldProbeSiteCount++];
        *row={}; row->returnRva=rva;row->code=code;row->unitId=hdr[2];
        row->thread=GetCurrentThreadId();
        PVOID frames[6]{};
        row->stackSize=CaptureStackBackTrace(1,6,frames,nullptr);
        for (unsigned n=0;n<row->stackSize;++n)
            row->stack[n]=reinterpret_cast<std::uintptr_t>(frames[n]);
    }
    if (row) {
        const auto now=GetTickCount64();
        const auto clickMs=WorldProbeLastClickMs.load(std::memory_order_acquire);
        ++row->hits;row->unitId=hdr[2];
        if (!row->firstMs)row->firstMs=now;
        row->lastMs=now;
        if (!clickMs || now<clickMs)++row->beforeClick;
        else if (WorldProbeEvidence::NearObservedClick(now,clickMs))
            ++row->nearClick;
        else ++row->afterClick;
        const auto witness=WorldProbeGround[hdr[2]%WorldProbeTrackedLimit]
            .load(std::memory_order_acquire);
        if(witness==WorldProbeToken(code,hdr[2]))++row->groundWitness;
        else ++row->otherGround;
    } else WorldProbeOverflow.fetch_add(1,std::memory_order_relaxed);
    WorldProbeMutex.unlock();
}
// Called only from the existing verified get-item-code hook. Never calls
// OriginalGetItemCode again or logs on a game/render thread. Item read is
// bounded by a live click window, verified ITEM type and monitored code.
void HiddenPickupObserveNativeItem(void* item,std::uint32_t rawCode,
                                   std::uintptr_t caller) noexcept {
    const auto click=PickupClickMs.load(std::memory_order_acquire);
    if(!click || !item || caller<Base || caller-Base>=ImageSize ||
       !WorldProbeMonitoredCode(CanonicalItemCode(rawCode)))return;
    const auto now=GetTickCount64();
    if(!HiddenPickupTracePolicy::InCaptureWindow(now,click,PickupWindowMs))return;
    const auto epoch=PickupClickEpoch.load(std::memory_order_acquire);
    std::array<std::uint32_t,3> hdr{};
    SIZE_T copied{};
    if(!ReadProcessMemory(GetCurrentProcess(),item,hdr.data(),sizeof(hdr),&copied)
       ||copied!=sizeof(hdr)||hdr[0]!=4||!hdr[2]) {
        PickupNativeReadFailures.fetch_add(1,std::memory_order_relaxed);return;
    }
    if(!PickupSitesMutex.try_lock()) {
        PickupNativeContention.fetch_add(1,std::memory_order_relaxed);return;
    }
    if(epoch!=PickupClickEpoch.load(std::memory_order_acquire) ||
       click!=PickupClickMs.load(std::memory_order_acquire)) {
        PickupSitesMutex.unlock();return;
    }
    const auto code=CanonicalItemCode(rawCode);
    const auto returnRva=caller-Base;
    PickupNativeSite* row=nullptr;
    for(std::size_t n=0;n<PickupSiteCount;++n)
        if(PickupSites[n].returnRva==returnRva &&
           PickupSites[n].unitId==hdr[2] && PickupSites[n].code==code){
            row=&PickupSites[n];break;
        }
    if(!row && PickupSiteCount<PickupSites.size()){
        row=&PickupSites[PickupSiteCount++];*row={};
        row->returnRva=returnRva;row->unitId=hdr[2];row->code=code;
        row->recentGroundWitness=WorldProbeGround[
            hdr[2]%WorldProbeTrackedLimit].load(std::memory_order_acquire)
            ==WorldProbeToken(code,hdr[2]);
    }
    if(row){
        ++row->hits;
        if(PickupSdkInventoryPollActive)++row->sdkInventoryPollHits;
        else ++row->outsideSdkPollHits;
        if(!row->firstMs)row->firstMs=now;
        row->lastMs=now;
        // Capture only the matching clicked candidate, once per code-reader
        // return site, with a strict per-click cap. Not a picker callback.
        if(!row->stackAttempted &&
           WorldProbeToken(code,hdr[2])==
               PickupHintToken.load(std::memory_order_acquire)){
            unsigned capturedRows{};
            for(std::size_t n=0;n<PickupSiteCount;++n)
                if(PickupSites[n].stackAttempted)++capturedRows;
            if(capturedRows<4){
                row->stackAttempted=true;
                void* frames[12]{};
                const auto depth=CaptureStackBackTrace(0,12,frames,nullptr);
                row->firstStackDepth=static_cast<unsigned>(depth);
                row->firstThreadId=GetCurrentThreadId();
                for(unsigned j=0;j<depth && j<row->firstStack.size();++j)
                    row->firstStack[j]=reinterpret_cast<std::uintptr_t>(frames[j]);
            }
        }
    } else PickupNativeOverflow.fetch_add(1,std::memory_order_relaxed);
    PickupSitesMutex.unlock();
}

// Read-only PE/unwind witness for offline disassembly of the code-reader
// callsites. A function containing a code read is NOT identified as a native
// world-model renderer or mouse picker by this check.
void WorldProbeNativeSite(std::uintptr_t returnRva) noexcept {
    if(returnRva<16 || returnRva>=ImageSize)return;
    std::array<std::uint8_t,24> bytes{};
    if(!ReadSafe(returnRva-12,bytes.data(),bytes.size()))return;
    char hex[sizeof(bytes)*3+1]{};
    for(std::size_t i=0;i<bytes.size();++i)
        std::snprintf(hex+3*i,sizeof(hex)-3*i,"%02X%s",
            static_cast<unsigned>(bytes[i]),i+1==bytes.size()?"":" ");
    DWORD64 ownerBase{};
    const auto* unwind=RtlLookupFunctionEntry(
        static_cast<DWORD64>(Base+returnRva-1),&ownerBase,nullptr);
    const bool owned=unwind && ownerBase==static_cast<DWORD64>(Base) &&
        unwind->BeginAddress<=returnRva-1 &&
        returnRva-1<unwind->EndAddress;
    char line[480]{};
    std::snprintf(line,sizeof(line),
        "LOOT_WORLD_PROBE_NATIVE_SITE returnRva=D2R+0x%llX "
        "functionBeginRva=0x%X functionEndRva=0x%X "
        "unwindOwned=%u bytesFromReturnMinus12=[%s] "
        "meaning=code-reader-caller-not-model-renderer-or-picker-proof",
        static_cast<unsigned long long>(returnRva),
        owned?static_cast<unsigned>(unwind->BeginAddress):0U,
        owned?static_cast<unsigned>(unwind->EndAddress):0U,
        owned?1U:0U,hex);
    Emit(line);
}
void WorldProbeSummary(int phase) noexcept {
    if (phase<0 || phase>=static_cast<int>(WorldProbePhaseLimit))return;
    char line[570]{};
    std::lock_guard lock(WorldProbeMutex);
    std::snprintf(line,sizeof(line),
        "LOOT_WORLD_PROBE_SUMMARY phase=%s codeReader=%llu badItem=%llu rows=%zu overflow=%llu contention=%llu groundedLabelSamples=%llu carriedHidden=%llu carriedControl=%llu lastHiddenId=%u lastControlId=%u sampledClickEdges=%u baselinePasses=%u postClickPasses=%u baselineMissing=%u clickSamplingMs=%u note=click-edge-is-sampled-not-native-input-and-carried-delta-is-temporal-not-pick-proof",
        WorldProbePhaseNames[static_cast<std::size_t>(phase)],
        static_cast<unsigned long long>(WorldProbeReads.load()),
        static_cast<unsigned long long>(WorldProbeReadFailures.load()),
        WorldProbeSiteCount,
        static_cast<unsigned long long>(WorldProbeOverflow.load()),
        static_cast<unsigned long long>(WorldProbeContended.load()),
        static_cast<unsigned long long>(WorldProbeGroundSamples.load()),
        static_cast<unsigned long long>(WorldProbeCarriedHidden.load()),
        static_cast<unsigned long long>(WorldProbeCarriedControl.load()),
        WorldProbeLastCarriedHidden.load(),WorldProbeLastCarriedControl.load(),
        WorldProbeClickEdges.load(),WorldProbeBaselinePasses.load(),
        WorldProbePostClickPasses.load(),WorldProbeMissedBaseline.load(),
        WorldProbeProbeSampleMs.load());
    Emit(line);
    std::snprintf(line,sizeof(line),
        "LOOT_WORLD_PROBE_LABEL_SUMMARY phase=%s labelCalls=%llu retained=%zu overflow=%llu contention=%llu source=native-C0420-label-unit-via-SoE-interop-or-standalone unitPointersRetained=0 pickerProof=0 rendererProof=0",
        WorldProbePhaseNames[static_cast<std::size_t>(phase)],
        static_cast<unsigned long long>(WorldProbeLabelCalls.load()),
        WorldProbeLabelCount,
        static_cast<unsigned long long>(WorldProbeLabelOverflow.load()),
        static_cast<unsigned long long>(WorldProbeLabelContention.load()));
    Emit(line);
    for(std::size_t n=0;n<WorldProbeLabelCount;++n){
        const auto& row=WorldProbeLabelUnits[n];
        char code[5]{};std::memcpy(code,&row.code,4);
        std::snprintf(line,sizeof(line),
            "LOOT_WORLD_PROBE_LABEL_UNIT phase=%s code=%.4s unitId=%u calls=%llu withText=%llu noText=%llu beforeClick=%llu nearClick=%llu afterClick=%llu firstMs=%llu lastMs=%llu role=confirmed-inworld-label-input-not-native-mouse-hit-test",
            WorldProbePhaseNames[static_cast<std::size_t>(phase)],
            code,row.unitId,static_cast<unsigned long long>(row.calls),
            static_cast<unsigned long long>(row.withText),
            static_cast<unsigned long long>(row.noText),
            static_cast<unsigned long long>(row.beforeClick),
            static_cast<unsigned long long>(row.nearClick),
            static_cast<unsigned long long>(row.afterClick),
            static_cast<unsigned long long>(row.firstMs),
            static_cast<unsigned long long>(row.lastMs));
        Emit(line);
    }
    std::snprintf(line,sizeof(line),
        "LOOT_WORLD_PROBE_LABEL_CALLER_SUMMARY phase=%s sampledStacks=%llu unmatched=%llu ambiguous=%llu dropped=%llu callerRows=%zu examples=%zu frameSource=CaptureStackBackTrace siteMatch=exact-D2R-return-RVA source=callback-sync-not-picker-proof",
        WorldProbePhaseNames[static_cast<std::size_t>(phase)],
        static_cast<unsigned long long>(WorldProbeStackSamples.load()),
        static_cast<unsigned long long>(WorldProbeStackUnmatched.load()),
        static_cast<unsigned long long>(WorldProbeStackAmbiguous.load()),
        static_cast<unsigned long long>(WorldProbeStackDropped.load()),
        WorldProbeLabelCallerCount,WorldProbeStackExampleCount);
    Emit(line);
    for(std::size_t n=0;n<WorldProbeLabelCallerCount;++n){
        const auto& caller=WorldProbeLabelCallers[n];
        char code[5]{};std::memcpy(code,&caller.code,4);
        std::snprintf(line,sizeof(line),
            "LOOT_WORLD_PROBE_LABEL_CALLER phase=%s code=%.4s unitId=%u returnRva=D2R+0x%llX samples=%llu nearSampledClick=%llu stackFrameIndex=%u meaning=formatter-caller-on-observer-stack-not-initial-hit-test-proof",
            WorldProbePhaseNames[static_cast<std::size_t>(phase)],
            code,caller.unitId,
            static_cast<unsigned long long>(caller.returnRva),
            static_cast<unsigned long long>(caller.samples),
            static_cast<unsigned long long>(caller.nearClick),
            caller.firstFrameIndex);
        Emit(line);
    }
    for(std::size_t n=0;n<WorldProbeStackExampleCount;++n){
        const auto& example=WorldProbeStackExamples[n];
        char code[5]{};std::memcpy(code,&example.code,4);
        char frameText[320]{};
        std::size_t used{};
        for(std::size_t i=0;i<example.frameRvas.size();++i){
            const auto written=std::snprintf(frameText+used,sizeof(frameText)-used,
                "%s%llX",i?",":"",
                static_cast<unsigned long long>(example.frameRvas[i]));
            if(written<=0 || static_cast<std::size_t>(written)>=sizeof(frameText)-used)
                break;
            used+=static_cast<std::size_t>(written);
        }
        std::snprintf(line,sizeof(line),
            "LOOT_WORLD_PROBE_LABEL_STACK_SAMPLE phase=%s code=%.4s unitId=%u depth=%u knownSite=D2R+0x%X otherKnownSites=%u first18D2rReturnRvas=[%s] zero=outside-D2R-or-missing",
            WorldProbePhaseNames[static_cast<std::size_t>(phase)],code,
            example.unitId,example.depth,example.matchedSite,
            example.otherMatchedSites,frameText);
        Emit(line);
    }
    for(std::size_t n=0;n<WorldProbeSiteCount;++n) {
        const auto& row=WorldProbeSites[n];
        char code[5]{};std::memcpy(code,&row.code,4);
        std::snprintf(line,sizeof(line),
            "LOOT_WORLD_PROBE_SITE phase=%s returnRva=D2R+0x%llX code=%.4s sampleUnitId=%u calls=%llu priorGroundLabelWitness=%llu noWitness=%llu beforeClick=%llu nearClick=%llu afterClick=%llu firstMs=%llu lastMs=%llu tid=%u stack0=D2R+0x%llX stack1=D2R+0x%llX qualification=code-helper-caller-only",
            WorldProbePhaseNames[static_cast<std::size_t>(phase)],
            static_cast<unsigned long long>(row.returnRva),code,row.unitId,
            static_cast<unsigned long long>(row.hits),
            static_cast<unsigned long long>(row.groundWitness),
            static_cast<unsigned long long>(row.otherGround),
            static_cast<unsigned long long>(row.beforeClick),
            static_cast<unsigned long long>(row.nearClick),
            static_cast<unsigned long long>(row.afterClick),
            static_cast<unsigned long long>(row.firstMs),
            static_cast<unsigned long long>(row.lastMs),row.thread,
            static_cast<unsigned long long>(row.stackSize && row.stack[0]>=Base && row.stack[0]<Base+ImageSize?
                row.stack[0]-Base:0),
            static_cast<unsigned long long>(row.stackSize>1 && row.stack[1]>=Base && row.stack[1]<Base+ImageSize?
                row.stack[1]-Base:0));
        Emit(line);
        bool first=true;
        for(std::size_t j=0;j<n;++j)
            if(WorldProbeSites[j].returnRva==row.returnRva){first=false;break;}
        if(first)WorldProbeNativeSite(row.returnRva);
    }
}
void WorldProbeAdvance() noexcept {
    const int former=WorldProbePhase.exchange(-1,std::memory_order_acq_rel);
    if (former>=0)WorldProbeSummary(former);
    const int next=former+1;
    if(next>=static_cast<int>(WorldProbePhaseLimit)) {
        Emit("LOOT_WORLD_PROBE_DONE all-phases-captured-no-gameplay-changes=1");
        return;
    }
    {
        std::lock_guard lock(WorldProbeMutex);
        WorldProbeSites.fill({});WorldProbeSiteCount=0;
        WorldProbeLabelUnits.fill({});WorldProbeLabelCount=0;
        WorldProbeLabelCallers.fill({});WorldProbeLabelCallerCount=0;
        WorldProbeStackExamples.fill({});WorldProbeStackExampleCount=0;
    }
    WorldProbeLabelCalls.store(0);WorldProbeLabelOverflow.store(0);
    WorldProbeLastStackCode.store(0);
    WorldProbeStackSamples.store(0);WorldProbeStackUnmatched.store(0);
    WorldProbeStackAmbiguous.store(0);WorldProbeStackDropped.store(0);
    WorldProbeLabelContention.store(0);
    WorldProbeReads.store(0);WorldProbeReadFailures.store(0);
    WorldProbeOverflow.store(0);WorldProbeContended.store(0);
    WorldProbeGroundSamples.store(0);
    WorldProbeCarriedHidden.store(0);WorldProbeCarriedControl.store(0);
    WorldProbeLastCarriedHidden.store(0);WorldProbeLastCarriedControl.store(0);
    WorldProbeLastClickMs.store(0);
    WorldProbeClickEdges.store(0);
    WorldProbeBaselinePasses.store(0);
    WorldProbePostClickPasses.store(0);
    WorldProbeMissedBaseline.store(0);
    WorldProbeBaselineReady.store(false,std::memory_order_release);
    WorldProbeEpoch.fetch_add(1,std::memory_order_acq_rel);
    for(auto& slot:WorldProbeBaseline)slot.store(0,std::memory_order_release);
    for(auto& slot:WorldProbeReported)slot.store(0,std::memory_order_release);
    WorldProbePhase.store(next,std::memory_order_release);
    char msg[300]{};
    std::snprintf(msg,sizeof(msg),
        "LOOT_WORLD_PROBE_PHASE_ENTER index=%d name=%s advance=Ctrl+Shift+F11 no-keyboard-visibility-dependency=1",
        next+1,WorldProbePhaseNames[static_cast<std::size_t>(next)]);
    Emit(msg);
}
struct WorldProbeInventoryPass final {
    int phase{-1};
    std::uint32_t epoch{};
    ULONGLONG sampledClickMs{};
    bool baseline{};
    std::uint32_t hidden{},control{};
};
D2RL::Inventory::IterationAction __cdecl WorldProbeInventoryItem(
    const D2RL::PluginContext*,const D2RL::Items::ItemInfo* info,
    void* userData) noexcept {
    auto* pass=static_cast<WorldProbeInventoryPass*>(userData);
    if (!pass || !WorldProbeRunning.load(std::memory_order_relaxed) ||
        WorldProbePhase.load(std::memory_order_acquire)!=pass->phase ||
        WorldProbeEpoch.load(std::memory_order_acquire)!=pass->epoch || !info ||
        info->structSize<D2RL::Items::ItemInfoRequiredSize ||
        info->container==D2RL::Items::ItemContainer::Ground ||
        !info->runtimeId)
        return D2RL::Inventory::IterationAction::Continue;
    const auto pos=info->runtimeId%WorldProbeTrackedLimit;
    const auto token=WorldProbeGround[pos].load(std::memory_order_acquire);
    if(static_cast<std::uint32_t>(token)!=info->runtimeId || !token)
        return D2RL::Inventory::IterationAction::Continue;
    const auto code=static_cast<std::uint32_t>(token>>32U);
    if(!WorldProbeMonitoredCode(code))
        return D2RL::Inventory::IterationAction::Continue;
    if(code==WorldProbeExaltedCode)++pass->hidden;
    else if(code==DivineCode)++pass->control;
    if(pass->baseline){
        const auto old=WorldProbeBaseline[pos].load(std::memory_order_relaxed);
        if(!old || old==token)WorldProbeBaseline[pos].store(token,std::memory_order_release);
        return D2RL::Inventory::IterationAction::Continue;
    }
    // A late first snapshot cannot distinguish an already-carried item from a
    // new pickup. Refuse to report a transition if pre-click baseline missed.
    if(!WorldProbeBaselineReady.load(std::memory_order_acquire) ||
        !pass->sampledClickMs ||
        WorldProbeBaseline[pos].load(std::memory_order_acquire)!=0)
        return D2RL::Inventory::IterationAction::Continue;
    if(WorldProbeReported[pos].exchange(token,std::memory_order_acq_rel)==token)
        return D2RL::Inventory::IterationAction::Continue;
    if(code==WorldProbeExaltedCode){
        WorldProbeCarriedHidden.fetch_add(1,std::memory_order_relaxed);
        WorldProbeLastCarriedHidden.store(info->runtimeId);
    }else{
        WorldProbeCarriedControl.fetch_add(1,std::memory_order_relaxed);
        WorldProbeLastCarriedControl.store(info->runtimeId);
    }
    char textCode[5]{};CodeText(code,textCode);
    char line[270]{};
    std::snprintf(line,sizeof(line),
        "LOOT_WORLD_PROBE_CARRIED_DELTA phase=%s code=%.4s unitId=%u "
        "baseline=absent current=carried sampledClickMs=%llu "
        "meaning=SDK-temporal-transition-not-native-click-target-proof",
        WorldProbePhaseNames[static_cast<std::size_t>(pass->phase)],
        textCode,info->runtimeId,
        static_cast<unsigned long long>(pass->sampledClickMs));
    Emit(line);
    return D2RL::Inventory::IterationAction::Continue;
}
void __cdecl WorldProbePollInventoryUi(const D2RL::PluginContext* context,
                                       void*) noexcept {
    const auto finish=[]() noexcept {
        WorldProbePollPending.store(false,std::memory_order_release);
    };
    const auto phase=WorldProbePhase.load(std::memory_order_acquire);
    if(!WorldProbeRunning.load(std::memory_order_acquire) || phase<0 ||
       !context || !SoundInventory || !SoundInventory->getLocalPlayer ||
       !SoundInventory->forEachInventoryItem){finish();return;}
    WorldProbeInventoryPass pass{};
    pass.phase=phase;
    pass.epoch=WorldProbeEpoch.load(std::memory_order_acquire);
    pass.sampledClickMs=WorldProbeLastClickMs.load(std::memory_order_acquire);
    pass.baseline=pass.sampledClickMs==0;
    D2RL::PlayerHandle player{};
    if(SoundInventory->getLocalPlayer(context,&player)!=
        D2RL::Inventory::Result::Success){finish();return;}
    constexpr auto mask=
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Equipment)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Belt)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Cube)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::PersonalStash)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::SharedStash)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::CustomPage);
    const D2RL::Inventory::ItemFilter filter{
        D2RL::Inventory::ItemFilterSize,0,mask,0};
    const auto result=SoundInventory->forEachInventoryItem(context,player,
        &filter,&WorldProbeInventoryItem,&pass);
    if(result!=D2RL::Inventory::Result::Success){finish();return;}
    if(SoundInventory->getCursorItem && SoundInventoryItems &&
       SoundInventoryItems->getItemInfo){
        D2RL::ItemHandle cursor{};
        if(SoundInventory->getCursorItem(context,player,&cursor)==
            D2RL::Inventory::Result::Success){
            D2RL::Items::ItemInfo info{};
            info.structSize=D2RL::Items::ItemInfoSize;
            if(SoundInventoryItems->getItemInfo(context,cursor,&info)==
                D2RL::Items::Result::Success &&
                info.container==D2RL::Items::ItemContainer::Cursor)
                (void)WorldProbeInventoryItem(context,&info,&pass);
        }
    }
    if(WorldProbeEpoch.load(std::memory_order_acquire)!=pass.epoch ||
       WorldProbePhase.load(std::memory_order_acquire)!=phase){
        finish();return;
    }
    if(pass.baseline){
        WorldProbeBaselineReady.store(true,std::memory_order_release);
        WorldProbeBaselinePasses.fetch_add(1,std::memory_order_relaxed);
    }else{
        WorldProbePostClickPasses.fetch_add(1,std::memory_order_relaxed);
        if(!WorldProbeBaselineReady.load(std::memory_order_acquire))
            WorldProbeMissedBaseline.fetch_add(1,std::memory_order_relaxed);
    }
    char line[300]{};
    std::snprintf(line,sizeof(line),
        "LOOT_WORLD_PROBE_SDK_SNAPSHOT phase=%s stage=%s "
        "trackedCarriedHidden=%u trackedCarriedControl=%u "
        "baselineReady=%u sampledClickMs=%llu "
        "note=SDK-inventory-and-cursor-with-prior-ground-identity-only",
        WorldProbePhaseNames[static_cast<std::size_t>(phase)],
        pass.baseline?"pre-click":"post-click",pass.hidden,pass.control,
        WorldProbeBaselineReady.load()?1U:0U,
        static_cast<unsigned long long>(pass.sampledClickMs));
    // Print only the first 3 snapshots before/after, not a log line every 125ms.
    if((pass.baseline?WorldProbeBaselinePasses.load():
          WorldProbePostClickPasses.load())<=3)Emit(line);
    finish();
}
// SDK possession is a secondary witness. Inventory/cursor transitions are
// not proof of which native click target was selected. Poll only through the
// SDK UI thread, never inspect inventory on the background/native hook thread.
struct HiddenPickupInventoryPass final {
    std::uint32_t id{};
    bool seen{};
};
D2RL::Inventory::IterationAction __cdecl HiddenPickupCheckCarried(
    const D2RL::PluginContext*,const D2RL::Items::ItemInfo* info,
    void* userData) noexcept {
    auto* pass=static_cast<HiddenPickupInventoryPass*>(userData);
    if(pass && info && info->structSize>=D2RL::Items::ItemInfoRequiredSize &&
       info->runtimeId==pass->id &&
       info->container!=D2RL::Items::ItemContainer::Ground)
        pass->seen=true;
    return D2RL::Inventory::IterationAction::Continue;
}
void __cdecl HiddenPickupPollUi(const D2RL::PluginContext* context,
                                void*) noexcept {
    const auto finish=[]() noexcept {
        PickupPollPending.store(false,std::memory_order_release);
    };
    const auto now=GetTickCount64();
    const auto click=PickupClickMs.load(std::memory_order_acquire);
    const auto token=click?PickupHintToken.load(std::memory_order_acquire):
        PickupHoverToken.load(std::memory_order_acquire);
    const auto hintMs=PickupHoverMs.load(std::memory_order_acquire);
    if(!context || !SoundInventory || !SoundInventory->getLocalPlayer ||
       !SoundInventory->forEachInventoryItem || !token ||
       (!click && !HiddenPickupTracePolicy::FreshHint(now,hintMs,1500))){
        finish();return;
    }
    // Exact dynamic extent: SDK getLocalPlayer, inventory iteration,
    // getCursorItem and getItemInfo. Destruction clears this TLS marker on
    // every early return, without marking unrelated UI work or other threads.
    const PickupSdkInventoryPollScope sdkPollScope{};
    const auto epoch=PickupClickEpoch.load(std::memory_order_acquire);
    D2RL::PlayerHandle player{};
    if(SoundInventory->getLocalPlayer(context,&player)!=
        D2RL::Inventory::Result::Success){finish();return;}
    HiddenPickupInventoryPass pass{static_cast<std::uint32_t>(token),false};
    constexpr auto mask=
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Inventory)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Equipment)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Belt)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::Cube)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::PersonalStash)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::SharedStash)|
        D2RL::Items::ContainerBit(D2RL::Items::ItemContainer::CustomPage);
    const D2RL::Inventory::ItemFilter filter{
        D2RL::Inventory::ItemFilterSize,0,mask,0};
    if(SoundInventory->forEachInventoryItem(context,player,&filter,
            &HiddenPickupCheckCarried,&pass)!=
        D2RL::Inventory::Result::Success){finish();return;}
    if(SoundInventory->getCursorItem && SoundInventoryItems &&
       SoundInventoryItems->getItemInfo){
        D2RL::ItemHandle cursor{};
        if(SoundInventory->getCursorItem(context,player,&cursor)==
            D2RL::Inventory::Result::Success){
            D2RL::Items::ItemInfo info{};
            info.structSize=D2RL::Items::ItemInfoSize;
            if(SoundInventoryItems->getItemInfo(context,cursor,&info)==
                D2RL::Items::Result::Success &&
                info.container==D2RL::Items::ItemContainer::Cursor)
                (void)HiddenPickupCheckCarried(context,&info,&pass);
        }
    }
    // No attribution across game joins or overlapping click windows.
    if(epoch!=PickupClickEpoch.load(std::memory_order_acquire)){
        finish();return;
    }
    const auto actualClick=PickupClickMs.load(std::memory_order_acquire);
    if(!actualClick && token==PickupHoverToken.load(std::memory_order_acquire)){
        PickupPreToken.store(token,std::memory_order_release);
        PickupPreStatus.store(pass.seen?2U:1U,std::memory_order_release);
        PickupPreMs.store(GetTickCount64(),std::memory_order_release);
        PickupPrePasses.fetch_add(1,std::memory_order_relaxed);
    }else if(actualClick==click && click &&
             token==PickupHintToken.load(std::memory_order_acquire)){
        PickupPostStatus.store(pass.seen?2U:1U,std::memory_order_release);
        PickupPostPasses.fetch_add(1,std::memory_order_relaxed);
        if(pass.seen){
            ULONGLONG expected{};
            (void)PickupPostFirstCarriedMs.compare_exchange_strong(expected,
                GetTickCount64(),std::memory_order_acq_rel);
        }
    }
    finish();
}
void HiddenPickupBeginClick(ULONGLONG now) noexcept {
    // The latest selected label input is just a candidate. A stale bulk
    // painter item MUST NOT become a pickup target if labels were toggled.
    auto token=PickupHoverToken.load(std::memory_order_acquire);
    const auto hoverMs=PickupHoverMs.load(std::memory_order_acquire);
    if(!HiddenPickupTracePolicy::FreshHint(now,hoverMs,1400)) token=0;
    if(!token){
        const auto bulkMs=HiddenGroundLastPainterSkipMs.load(std::memory_order_acquire);
        if(HiddenPickupTracePolicy::FreshHint(now,bulkMs,900)) {
            const auto code=HiddenGroundLastPainterSkipCode.load(std::memory_order_relaxed);
            const auto id=HiddenGroundLastPainterSkipId.load(std::memory_order_relaxed);
            if(id && WorldProbeMonitoredCode(code))token=WorldProbeToken(code,id);
        }
    }
    if(!token)return;
    if(PickupClickMs.load(std::memory_order_acquire))return;
    PickupClickEpoch.fetch_add(1,std::memory_order_acq_rel);
    PickupHintToken.store(token,std::memory_order_release);
    const bool baseline=PickupPreToken.load(std::memory_order_acquire)==token &&
        HiddenPickupTracePolicy::FreshHint(now,
            PickupPreMs.load(std::memory_order_acquire),1600);
    PickupBaselineAtClick.store(baseline?
        PickupPreStatus.load(std::memory_order_acquire):0U,
        std::memory_order_release);
    PickupPostStatus.store(0,std::memory_order_release);
    PickupPostFirstCarriedMs.store(0,std::memory_order_release);
    PickupBaselinePassesAtClick.store(PickupPrePasses.load(
        std::memory_order_acquire),std::memory_order_release);
    PickupPostPasses.store(0,std::memory_order_release);
    PickupNativeReadFailures.store(0,std::memory_order_release);
    PickupNativeContention.store(0,std::memory_order_release);
    PickupNativeOverflow.store(0,std::memory_order_release);
    {
        std::lock_guard lock(PickupSitesMutex);
        PickupSites.fill({});PickupSiteCount=0;
    }
    PickupClickMs.store(now,std::memory_order_release);
    char code[5]{};CodeText(static_cast<std::uint32_t>(token>>32U),code);
    char line[370]{};
    std::snprintf(line,sizeof(line),
        "LOOT_PICKUP_TRACE_CLICK version=1.0.0 sampledMs=%llu "
        "candidate=%.4s unitId=%u baseline=%u "
        "source=recent-inworld-label-or-bulk-identity "
        "nativeClickTarget=UNKNOWN inputSuppression=0",
        static_cast<unsigned long long>(now),code,
        static_cast<std::uint32_t>(token),
        PickupBaselineAtClick.load(std::memory_order_acquire));
    Emit(line);
}
void HiddenPickupFinishClick(ULONGLONG now) noexcept {
    auto click=PickupClickMs.load(std::memory_order_acquire);
    if(!click || !HiddenPickupTracePolicy::CaptureFinished(now,click,
        PickupWindowMs+200))return;
    if(!PickupClickMs.compare_exchange_strong(click,0,
        std::memory_order_acq_rel))return;
    const auto token=PickupHintToken.load(std::memory_order_acquire);
    std::array<PickupNativeSite,PickupSitesLimit> rows{};
    std::size_t rowCount{};
    {
        std::lock_guard lock(PickupSitesMutex);
        rowCount=PickupSiteCount;
        for(std::size_t i=0;i<rowCount;++i)rows[i]=PickupSites[i];
    }
    std::uint64_t sdkPollReads=0,outsideSdkPollReads=0;
    for(std::size_t i=0;i<rowCount;++i){
        sdkPollReads+=rows[i].sdkInventoryPollHits;
        outsideSdkPollReads+=rows[i].outsideSdkPollHits;
    }
    char code[5]{};CodeText(static_cast<std::uint32_t>(token>>32U),code);
    char msg[610]{};
    std::snprintf(msg,sizeof(msg),
        "LOOT_PICKUP_TRACE_SUMMARY version=1.0.0 candidate=%.4s unitId=%u "
        "baseline=%u post=%u firstCarriedMs=%llu prePasses=%u postPasses=%u "
        "nativeCodeReaderSites=%zu sdkPollReads=%llu outsideSdkPollReads=%llu "
        "readFailures=%llu contention=%llu overflow=%llu "
        "meaning=SDK-poll-excluded-NOT-native-selection-proof",
        code,static_cast<std::uint32_t>(token),
        PickupBaselineAtClick.load(std::memory_order_acquire),
        PickupPostStatus.load(std::memory_order_acquire),
        static_cast<unsigned long long>(PickupPostFirstCarriedMs.load()),
        PickupBaselinePassesAtClick.load(),PickupPostPasses.load(),rowCount,
        static_cast<unsigned long long>(sdkPollReads),
        static_cast<unsigned long long>(outsideSdkPollReads),
        static_cast<unsigned long long>(PickupNativeReadFailures.load()),
        static_cast<unsigned long long>(PickupNativeContention.load()),
        static_cast<unsigned long long>(PickupNativeOverflow.load()));
    Emit(msg);
    for(std::size_t i=0;i<rowCount;++i){
        const auto& r=rows[i];char siteCode[5]{};CodeText(r.code,siteCode);
        std::snprintf(msg,sizeof(msg),
            "LOOT_PICKUP_TRACE_READER_SITE version=1.0.0 code=%.4s "
            "unitId=%u callerReturnRva=D2R+0x%llX calls=%llu "
            "firstDeltaMs=%llu lastDeltaMs=%llu "
            "sdkPollHits=%llu outsideSdkPollHits=%llu "
            "recentGroundWitness=%u matchesCandidate=%u "
            "role=code-reader-origin-witness-not-proven-picker",
            siteCode,r.unitId,
            static_cast<unsigned long long>(r.returnRva),
            static_cast<unsigned long long>(r.hits),
            static_cast<unsigned long long>(r.firstMs-click),
            static_cast<unsigned long long>(r.lastMs-click),
            static_cast<unsigned long long>(r.sdkInventoryPollHits),
            static_cast<unsigned long long>(r.outsideSdkPollHits),
            r.recentGroundWitness?1U:0U,
            WorldProbeToken(r.code,r.unitId)==token?1U:0U);
        Emit(msg);
    }
    // Reconstruct frames on the worker, never emit inside a game hook. A
    // zero frame RVA means an address outside D2R (including Loader/SoE),
    // or an unavailable frame; no invented function/unwind boundaries.
    for(std::size_t i=0;i<rowCount;++i){
        const auto& row=rows[i];
        if(!row.firstStackDepth ||
           WorldProbeToken(row.code,row.unitId)!=token)continue;
        std::string frames;
        unsigned outside{};
        for(unsigned j=0;j<row.firstStackDepth &&
                j<row.firstStack.size();++j){
            const auto addr=row.firstStack[j];
            const bool inD2r=addr>=Base && addr-Base<ImageSize;
            if(!inD2r)++outside;
            char component[38]{};
            std::snprintf(component,sizeof(component),"%s%llX",
                frames.empty()?"":",",
                static_cast<unsigned long long>(inD2r?addr-Base:0));
            frames+=component;
        }
        char line[570]{};
        std::snprintf(line,sizeof(line),
            "LOOT_PICKUP_TRACE_CALLSTACK version=1.0.0 "
            "code=%.4s unitId=%u returnRva=D2R+0x%llX thread=%u "
            "depth=%u nonD2rFrames=%u rvas=[%s] "
            "meaning=item-code-read-stack-not-picker-proof",
            code,row.unitId,
            static_cast<unsigned long long>(row.returnRva),
            row.firstThreadId,row.firstStackDepth,outside,frames.c_str());
        Emit(line);
    }
    // Prefer this click's candidate-unit code-reader sites (including the
    // formerly truncated D2R+0x6624D2). These are STILL not verified picker
    // callsites. At most eight tiny read-only instruction windows.
    std::array<std::uintptr_t,8> unique{};std::size_t count{};
    for(int priority=0;priority<2 && count<unique.size();++priority){
        for(std::size_t i=0;i<rowCount && count<unique.size();++i){
            const auto& row=rows[i];
            const bool match=WorldProbeToken(row.code,row.unitId)==token;
            if(match!=(priority==0))continue;
            const auto rva=row.returnRva;
            bool seen=false;
            for(std::size_t j=0;j<count;++j)if(unique[j]==rva)seen=true;
            if(!seen){
                unique[count++]=rva;
                WorldProbeNativeSite(rva);
                // Extend the previous 24-byte window to 128 bytes, for up
                // to three matching candidate sites only. Runtime .text is
                // read-only; we never install hooks at these unqualified RVAs.
                if(match && rva>=64 && rva+64<=ImageSize){
                    std::array<std::uint8_t,128> codeWindow{};
                    if(ReadSafe(rva-64,codeWindow.data(),codeWindow.size())){
                        for(std::size_t offset=0;offset<codeWindow.size();offset+=16){
                            char hex[16*3+1]{};
                            for(std::size_t b=0;b<16;++b){
                                const auto pos=b*3;
                                std::snprintf(hex+pos,sizeof(hex)-pos,
                                    "%02X%s",codeWindow[offset+b],
                                    b==15?"":" ");
                            }
                            char line[300]{};
                            std::snprintf(line,sizeof(line),
                                "LOOT_PICKUP_TRACE_CODE_WINDOW version=1.0.0 "
                                "returnRva=D2R+0x%llX startRva=D2R+0x%llX "
                                "bytes='%s' qualifier=read-only-not-disassembly",
                                static_cast<unsigned long long>(rva),
                                static_cast<unsigned long long>(rva-64+offset),hex);
                            Emit(line);
                        }
                    }
                }
            }
        }
    }
}
void HiddenPickupPump(ULONGLONG now) noexcept {
    HiddenPickupFinishClick(now);
    if(!Context || !SoundThreads || !SoundThreads->runOnUiThread ||
       !SoundInventory || !SoundInventory->getLocalPlayer ||
       !SoundInventory->forEachInventoryItem ||
       PickupPollPending.load(std::memory_order_acquire))return;
    const auto click=PickupClickMs.load(std::memory_order_acquire);
    const auto hover=PickupHoverMs.load(std::memory_order_acquire);
    if(!click && !HiddenPickupTracePolicy::FreshHint(now,hover,1500))return;
    const auto last=PickupPollScheduledMs.load(std::memory_order_acquire);
    if(now<last || now-last<140)return;
    if(PickupPollPending.exchange(true,std::memory_order_acq_rel))return;
    PickupPollScheduledMs.store(now,std::memory_order_release);
    if(SoundThreads->runOnUiThread(Context,&HiddenPickupPollUi,nullptr)!=
        D2RL::Threads::Result::Success)
        PickupPollPending.store(false,std::memory_order_release);
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
// The best currently verified downstream unit-consumer is the in-world
// formatter D2R+0xC0420. Discover its potential *direct* callers in build
// 93847 executable .text, bounded and read-only. The byte pattern E8-rel32
// is not an instruction-boundary proof; offline disassembly is mandatory.
void WorldProbeScanLabelEntryCallers() noexcept {
    if(!Base || !ImageSize || !Context || !D2RL::GetBuildName(Context) ||
       std::string_view(D2RL::GetBuildName(Context))!="93847")return;
    IMAGE_DOS_HEADER dos{};
    if(!ReadSafe(0,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
       dos.e_lfanew<=0 || dos.e_lfanew>0x1000){
        Emit("LOOT_WORLD_PROBE_LABEL_XREF_UNAVAILABLE reason=pe-dos-header");return;
    }
    IMAGE_NT_HEADERS64 nt{};
    if(!ReadSafe(static_cast<std::uintptr_t>(dos.e_lfanew),&nt,sizeof(nt)) ||
       nt.Signature!=IMAGE_NT_SIGNATURE ||
       nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
       !nt.FileHeader.NumberOfSections ||
       nt.FileHeader.NumberOfSections>96){
        Emit("LOOT_WORLD_PROBE_LABEL_XREF_UNAVAILABLE reason=pe-nt-header");return;
    }
    const auto sectionsRva=static_cast<std::uintptr_t>(dos.e_lfanew)+
        sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
    std::array<IMAGE_SECTION_HEADER,96> sections{};
    if(!ReadSafe(sectionsRva,sections.data(),
            static_cast<std::size_t>(nt.FileHeader.NumberOfSections)*
            sizeof(IMAGE_SECTION_HEADER))){
        Emit("LOOT_WORLD_PROBE_LABEL_XREF_UNAVAILABLE reason=pe-sections");return;
    }
    std::uintptr_t begin{};std::size_t size{};
    for(std::size_t i=0;i<nt.FileHeader.NumberOfSections;++i){
        const auto& section=sections[i];
        if(std::memcmp(section.Name,".text",5) ||
           !(section.Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        begin=section.VirtualAddress;size=section.Misc.VirtualSize;break;
    }
    if(!begin || begin>=ImageSize || size<5 ||
       size>0x8000000U || size>ImageSize-begin){
        Emit("LOOT_WORLD_PROBE_LABEL_XREF_UNAVAILABLE reason=text-section-bounds");return;
    }
    constexpr std::size_t chunk=0x4000, maxPrinted=16;
    std::vector<std::uint8_t> data{};
    try{data.resize(chunk+4);}catch(const std::exception&){
        Emit("LOOT_WORLD_PROBE_LABEL_XREF_UNAVAILABLE reason=allocation");return;
    }
    std::size_t candidates{},printed{},failed{};
    const auto end=begin+size;
    for(auto at=begin;at<end;at+=chunk){
        const auto actual=std::min<std::size_t>(chunk+4,end-at);
        if(actual<5 || !ReadSafe(at,data.data(),actual)){++failed;continue;}
        const auto count=std::min<std::size_t>(chunk,actual-4);
        for(std::size_t i=0;i<count;++i){
            if(data[i]!=0xE8)continue;
            std::int32_t displacement{};
            std::memcpy(&displacement,data.data()+i+1,sizeof(displacement));
            const auto target=static_cast<std::int64_t>(at+i+5)+
                static_cast<std::int64_t>(displacement);
            if(target!=static_cast<std::int64_t>(InWorldFormatterRva))continue;
            ++candidates;
            if(printed>=maxPrinted)continue;
            const auto site=at+i;
            char line[480]{};
            std::snprintf(line,sizeof(line),
                "LOOT_WORLD_PROBE_LABEL_XREF callRva=D2R+0x%llX returnRva=D2R+0x%llX target=D2R+0xC0420 rawE8Candidate=1 aligned=unverified picker=unverified modelRenderer=unverified",
                static_cast<unsigned long long>(site),
                static_cast<unsigned long long>(site+5));
            Emit(line);
            constexpr std::size_t pre=32,post=48;
            if(site>=pre && site+post<=ImageSize){
                std::array<std::uint8_t,pre+post> window{};
                if(ReadSafe(site-pre,window.data(),window.size())){
                    for(std::size_t offset=0;offset<window.size();offset+=16){
                        char hex[49]{};
                        for(std::size_t k=0;k<16;++k)
                            std::snprintf(hex+k*3,sizeof(hex)-k*3,
                                "%02X ",unsigned(window[offset+k]));
                        std::snprintf(line,sizeof(line),
                            "LOOT_WORLD_PROBE_LABEL_XREF_BYTES callRva=D2R+0x%llX rva=D2R+0x%llX hex='%s'",
                            static_cast<unsigned long long>(site),
                            static_cast<unsigned long long>(site-pre+offset),hex);
                        Emit(line);
                    }
                }
            }
            ++printed;
        }
    }
    char line[340]{};
    std::snprintf(line,sizeof(line),
        "LOOT_WORLD_PROBE_LABEL_XREF_SUMMARY candidates=%zu printed=%zu failedChunks=%zu target=D2R+0xC0420 scan=executable-text-only indirectCallersUnobserved=1 noHooks=1 instructionAlignmentUnverified=1",
        candidates,printed,failed);
    Emit(line);
}
// Static instruction-context witness only. These three calls are the exact
// E8-rel32 byte candidates observed in 0.2.17; this bounded read does not
// claim a verified containing function or dereference a candidate unit.
void WorldProbeDumpLabelCallerContext() noexcept {
    constexpr std::array<std::uintptr_t,3> sites{{0x880AA5,0x880AE3,0x880B0C}};
    if(!Base || ImageSize<0x880B60)return;
    for(const auto site:sites){
        std::array<std::uint8_t,5> bytes{};
        if(!ReadSafe(site,bytes.data(),bytes.size()) || bytes[0]!=0xE8){
            Emit("LOOT_WORLD_PROBE_CALLER_CONTEXT_UNAVAILABLE reason=unexpected-call-byte");return;
        }
        std::int32_t displacement{};
        std::memcpy(&displacement,bytes.data()+1,sizeof(displacement));
        if(static_cast<std::int64_t>(site+5)+displacement !=
           static_cast<std::int64_t>(InWorldFormatterRva)){
            Emit("LOOT_WORLD_PROBE_CALLER_CONTEXT_UNAVAILABLE reason=call-target-mismatch");return;
        }
    }
    constexpr std::uintptr_t begin=0x880960, end=0x880B60;
    char line[440]{};
    std::snprintf(line,sizeof(line),
        "LOOT_WORLD_PROBE_CALLER_CONTEXT_READY beginRva=D2R+0x%llX endRva=D2R+0x%llX directCalls=3 readOnly=1 disassembly=required modelRenderer=unknown picker=unknown",
        static_cast<unsigned long long>(begin),
        static_cast<unsigned long long>(end));
    Emit(line);
    for(auto rva=begin;rva<end;rva+=16){
        std::array<std::uint8_t,16> bytes{};
        if(!ReadSafe(rva,bytes.data(),bytes.size())){
            Emit("LOOT_WORLD_PROBE_CALLER_CONTEXT_UNAVAILABLE reason=window-read");return;
        }
        char hex[49]{};
        for(std::size_t i=0;i<bytes.size();++i)
            std::snprintf(hex+i*3,sizeof(hex)-i*3,"%02X ",
                static_cast<unsigned>(bytes[i]));
        std::snprintf(line,sizeof(line),
            "LOOT_WORLD_PROBE_CALLER_CONTEXT_BYTES rva=D2R+0x%llX hex='%s'",
            static_cast<unsigned long long>(rva),hex);
        Emit(line);
    }
}
// 0.2.19: Analyze the immediate predecessor call chain in the EXISTING
// label-producing path. We already know C0420 is a label formatter, not the
// world renderer and not the native pickup target. The 0.2.18 code window
// shows that each of its three direct callers receives a value from a
// distinct preceding CALL. Verify all E8-rel32 targets on the LIVE 93847
// module before printing bounded callee-entry windows for offline analysis.
// No guessed signature, detour, unit pointer dereference, or gameplay write.
void WorldProbeDumpUpstreamCandidates() noexcept {
    if(!Base || !Context || !ImageSize ||
       !D2RL::GetBuildName(Context) ||
       std::string_view(D2RL::GetBuildName(Context))!="93847") {
        Emit("LOOT_WORLD_PROBE_UPSTREAM_UNAVAILABLE reason=build-or-image");
        return;
    }
    for(const auto& branch:WorldProbeUpstream::Branches) {
        const std::array<std::uintptr_t,3> sites{{
            branch.sourceCall,branch.producerCall,branch.formatterCall}};
        const std::array<std::uintptr_t,3> targets{{
            branch.expectedSource,branch.expectedProducer,
            InWorldFormatterRva}};
        for(std::size_t i=0;i<sites.size();++i) {
            std::array<std::uint8_t,5> bytes{};
            if(!ReadSafe(sites[i],bytes.data(),bytes.size())) {
                char line[260]{};
                std::snprintf(line,sizeof(line),
                    "LOOT_WORLD_PROBE_UPSTREAM_REFUSED branch=%s stage=%zu reason=unreadable-callsite rva=D2R+0x%llX",
                    branch.name,i,
                    static_cast<unsigned long long>(sites[i]));
                Emit(line);return;
            }
            std::uintptr_t target{};
            if(!WorldProbeUpstream::DecodeRelCall(bytes,sites[i],target) ||
               target!=targets[i]) {
                char line[280]{};
                std::snprintf(line,sizeof(line),
                    "LOOT_WORLD_PROBE_UPSTREAM_REFUSED branch=%s stage=%zu reason=changed-direct-call rva=D2R+0x%llX expectedTarget=D2R+0x%llX actualTarget=D2R+0x%llX",
                    branch.name,i,
                    static_cast<unsigned long long>(sites[i]),
                    static_cast<unsigned long long>(targets[i]),
                    static_cast<unsigned long long>(target));
                Emit(line);return;
            }
        }
        // Only assert exact register-move bytes where shown by the captured
        // 0.2.18 native sequence. This is static value-flow evidence, NOT a
        // verified C++ return signature or initial mouse hit-test contract.
        std::array<std::uint8_t,3> forward{};
        if(!ReadSafe(branch.forwardRva,forward.data(),forward.size()) ||
           forward!=branch.expectedForward) {
            char line[260]{};
            std::snprintf(line,sizeof(line),
                "LOOT_WORLD_PROBE_UPSTREAM_REFUSED branch=%s reason=changed-value-forward rva=D2R+0x%llX",
                branch.name,
                static_cast<unsigned long long>(branch.forwardRva));
            Emit(line);return;
        }
    }
    Emit("LOOT_WORLD_PROBE_UPSTREAM_READY version=1.0.0 branches=3 callsVerified=9 valueForwards=3 mode=read-only newHooks=0 modelRenderer=unknown mousePicker=unknown nativeItemWrites=0");
    for(const auto& branch:WorldProbeUpstream::Branches) {
        char line[420]{};
        std::snprintf(line,sizeof(line),
            "LOOT_WORLD_PROBE_UPSTREAM_BRANCH name=%s sourceCall=D2R+0x%llX sourceTarget=D2R+0x%llX producerCall=D2R+0x%llX producerTarget=D2R+0x%llX forwardAt=D2R+0x%llX formatterCall=D2R+0x%llX formatterTarget=D2R+0xC0420 qualification=verified-live-E8-and-value-forward producerRole=unit-input-lead-not-native-picker-proof",
            branch.name,
            static_cast<unsigned long long>(branch.sourceCall),
            static_cast<unsigned long long>(branch.expectedSource),
            static_cast<unsigned long long>(branch.producerCall),
            static_cast<unsigned long long>(branch.expectedProducer),
            static_cast<unsigned long long>(branch.forwardRva),
            static_cast<unsigned long long>(branch.formatterCall));
        Emit(line);
    }
    for(const auto& target:WorldProbeUpstream::Targets) {
        constexpr std::size_t windowSize=128;
        if(target.rva>=ImageSize || windowSize>ImageSize-target.rva) {
            char line[240]{};
            std::snprintf(line,sizeof(line),
                "LOOT_WORLD_PROBE_UPSTREAM_TARGET_UNAVAILABLE name=%s rva=D2R+0x%llX reason=image-range",
                target.name,static_cast<unsigned long long>(target.rva));
            Emit(line);continue;
        }
        std::array<std::uint8_t,windowSize> bytes{};
        if(!ReadSafe(target.rva,bytes.data(),bytes.size())) {
            char line[240]{};
            std::snprintf(line,sizeof(line),
                "LOOT_WORLD_PROBE_UPSTREAM_TARGET_UNAVAILABLE name=%s rva=D2R+0x%llX reason=read-failed",
                target.name,static_cast<unsigned long long>(target.rva));
            Emit(line);continue;
        }
        DWORD64 ownerBase{};
        const auto* unwind=RtlLookupFunctionEntry(
            static_cast<DWORD64>(Base+target.rva),&ownerBase,nullptr);
        const bool owned=unwind && ownerBase==static_cast<DWORD64>(Base) &&
            unwind->BeginAddress<=target.rva && target.rva<unwind->EndAddress;
        char line[420]{};
        std::snprintf(line,sizeof(line),
            "LOOT_WORLD_PROBE_UPSTREAM_TARGET name=%s rva=D2R+0x%llX entry=[%02X %02X %02X %02X %02X] beginRva=D2R+0x%X endRva=D2R+0x%X unwindOwned=%u entryKind=%s readOnly=1 requiredNext=disassemble-and-confirm-native-ABI",
            target.name,static_cast<unsigned long long>(target.rva),
            static_cast<unsigned>(bytes[0]),static_cast<unsigned>(bytes[1]),
            static_cast<unsigned>(bytes[2]),static_cast<unsigned>(bytes[3]),
            static_cast<unsigned>(bytes[4]),
            owned?static_cast<unsigned>(unwind->BeginAddress):0U,
            owned?static_cast<unsigned>(unwind->EndAddress):0U,owned?1U:0U,
            bytes[0]==0xFF && bytes[1]==0x25?"rip-indirect-bridge":
            bytes[0]==0xE9?"jmp-bridge":"no-obvious-entry-jump");
        Emit(line);
        for(std::size_t offset=0;offset<bytes.size();offset+=16) {
            char hex[49]{};
            for(std::size_t n=0;n<16;++n)
                std::snprintf(hex+n*3,sizeof(hex)-n*3,"%02X ",
                    static_cast<unsigned>(bytes[offset+n]));
            std::snprintf(line,sizeof(line),
                "LOOT_WORLD_PROBE_UPSTREAM_BYTES name=%s rva=D2R+0x%llX hex='%s'",
                target.name,static_cast<unsigned long long>(target.rva+offset),hex);
            Emit(line);
        }
    }
    Emit("LOOT_WORLD_PROBE_UPSTREAM_DONE review-targets=0xF1900,0x18D960,0x18DCA0 no-abi-assumption=1 no-native-hooks=1");
}
void RuntimeWorkerStart() noexcept {
    if(WorldProbeWorker.joinable()) return;
    try {
        WorldProbeWorker=std::jthread([](std::stop_token stop) noexcept {
            RuntimeWorkerLoop(stop);
        });
    } catch(const std::exception&) {
        Emit("LOOT_FILTER_INACTIVE runtime-worker-create-failed=1 liveReload=0");
    }
}
void RuntimeWorkerStop() noexcept {
    if(WorldProbeWorker.joinable()) {
        WorldProbeWorker.request_stop();
        WorldProbeWorker.join();
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
    MinimapProbeArmed.store(false,std::memory_order_release);
    { std::lock_guard lock(MinimapProbeMutex);
      MinimapProbeRows.fill({});MinimapProbeCount=0;MinimapProbeReported=0; }
    MinimapProbeContended.store(0,std::memory_order_relaxed);
    LootLatencyArmed.store(false,std::memory_order_release);
    GroundPropertyEvidenceArmed.store(false,std::memory_order_release);
    SocketProbeArmed.store(false,std::memory_order_release);
    EtherealProbeArmed.store(false,std::memory_order_release);
    MinimapProbeArmed.store(false,std::memory_order_release);
    { std::lock_guard lock(EtherealProbeMutex);
      EtherealProbeRows.fill({});EtherealProbeCount=0; }
    EtherealProbeContended.store(0);
    { std::lock_guard lock(SocketProbeMutex);
      SocketProbeRows.fill({});SocketProbeCount=0; }
    SocketProbeContention.store(0);
    { std::lock_guard lock(GroundPropertyEvidenceMutex);
      GroundPropertyEvidenceRows.fill({});
      GroundPropertyEvidenceCount=0; GroundPropertyEvidenceReported=0; }
    GroundPropertyEvidenceReadFailed.store(0);
    GroundPropertyEvidenceContended.store(0);
    HoverEventArmed.store(false);HoverEventDeadline.store(0);
    HoverEventItems.store(nullptr,std::memory_order_release);HoverEventBus=nullptr;
    HoverEventListener=D2RL::SharedEvents::InvalidHandle;
    UiTextArmed.store(false);UiTextDeadline.store(0);
    UiTextCalls.store(0);UiTextKnownReturnCalls.store(0);
    UiTextOtherReturnCalls.store(0);UiTextContended.store(0);
    UiTextOverflow.store(0);UiTextPhase.fill(0);
    UiTextSites.fill({});UiTextSiteCount=0;
    TooltipProducerArmed.store(false,std::memory_order_relaxed);
    TooltipProducerDeadline.store(0,std::memory_order_relaxed);
    TooltipProducerCalls.store(0);TooltipProducerSamples.store(0);
    TooltipProducerContended.store(0);TooltipProducerOverflow.store(0);
    for(auto& count:TooltipProducerSiteCalls) count.store(0);
    TooltipProducerOtherCalls.store(0);
    TooltipProducerPhase.fill(0);TooltipProducerRows.fill({});
    TooltipProducerRowCount=0;TooltipProducerInstalled.store(false);
    OriginalTooltipProducer=nullptr;
    TooltipRouteArmed.store(false,std::memory_order_relaxed);
    TooltipRouteDeadline.store(0,std::memory_order_relaxed);
    TooltipRouteHits.store(0);TooltipRouteSkipped.store(0);
    TooltipRouteContended.store(0);TooltipRouteOverflow.store(0);
    TooltipRoutePhase.fill(0);TooltipRouteRows.fill({});TooltipRouteCount=0;
    HoverProbeArmed.store(false,std::memory_order_relaxed);
    HoverProbeDeadline.store(0,std::memory_order_relaxed);
    HoverProbePhase.fill(0);
    for(auto& group:HoverProbeCallers) group.fill({});
    HoverProbeSizes.fill(0);HoverProbeHits.fill(0);HoverProbeOverflow.fill(0);
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
        ArmMinimapTracking();
    }

    CodeBridgeArmed.store(false, std::memory_order_relaxed);
    CodeBridgeAttempts.store(0, std::memory_order_relaxed);
    CodeBridgeSuccess.store(0, std::memory_order_relaxed);
    CodeBridgeGuardReject.store(0, std::memory_order_relaxed);
    CodeBridgeInvalid.store(0, std::memory_order_relaxed);
    CodeBridgeDivine.store(0, std::memory_order_relaxed);
    CodeBridgeMap.store(0, std::memory_order_relaxed);
    CodeBridgeOther.store(0, std::memory_order_relaxed);
    CodeBridgeDivineMismatch.store(0, std::memory_order_relaxed);
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
    CodeRenameArmed.store(false, std::memory_order_relaxed);
    CodeRenameQualified.store(0, std::memory_order_relaxed);
    CodeRenameIdMismatch.store(0, std::memory_order_relaxed);
    CodeRenameLookups.store(0, std::memory_order_relaxed);
    CodeRenameInvalid.store(0, std::memory_order_relaxed);
    CodeRenameNoRule.store(0, std::memory_order_relaxed);
    CodeRenameRuleMatches.store(0, std::memory_order_relaxed);
    CodeRenameTextLengthMismatch.store(0, std::memory_order_relaxed);
    CodeRenameReadFailures.store(0, std::memory_order_relaxed);
    CodeRenameWrites.store(0, std::memory_order_relaxed);
    RenameArmed.store(false, std::memory_order_relaxed);
    RenameQualified.store(0, std::memory_order_relaxed);
    RenameNonDivine.store(0, std::memory_order_relaxed);
    RenameIdMismatch.store(0, std::memory_order_relaxed);
    RenameTextMismatch.store(0, std::memory_order_relaxed);
    RenameReadFailures.store(0, std::memory_order_relaxed);
    RenameWrites.store(0, std::memory_order_relaxed);
    FormatterHookInstalled.store(false, std::memory_order_relaxed);
    FormatterCalls.store(0,std::memory_order_relaxed);
    FormatterContention.store(0,std::memory_order_relaxed);
    CollectionHookInstalled.store(false, std::memory_order_relaxed);
    CollectionTotal.store(0, std::memory_order_relaxed);
    CollectionContended.store(0, std::memory_order_relaxed);
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
    (void)InstallStandaloneAutomapProjectionObserver();
    // Reuse the qualified native filter pipeline automatically once the
    // complete JSON ruleset is published and the item-code reader is ready.
    if (std::atomic_load_explicit(&PublishedFilterRules,
            std::memory_order_acquire))
        (void)ActivateConfiguredFilter(true);
    else Emit("LOOT_FILTER_AUTO_INACTIVE reason=json-absent-or-invalid no-ground-label-feature-hooks=1");
    RegisterInWorldLifecycle();
    StartSoundInventoryObserver();
    NativeActionInstall();
    RuntimeWorkerStart();

    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    RuntimeWorkerStop();
    AutomapProjectionArmed.store(false,std::memory_order_release);
    MinimapOverlayRenderer::Shutdown();
    AutomapProjectionHookInstalled.store(false,std::memory_order_release);
    FilterLiveReloadAvailable.store(false,std::memory_order_release);
    NativeActionPhase.store(-1,std::memory_order_release);
    NativePickupGuardQualified.store(false,std::memory_order_release);
    GroundQuantityReader.store(nullptr,std::memory_order_release);
    HideGroundArmed.store(false,std::memory_order_release);
    ResetNativeRowFontColor();
    NativeRowBgLiveEnabled.store(false,std::memory_order_release);
    NativeRowBgLiveEpoch.fetch_add(1,std::memory_order_acq_rel);
    NativeRowBgTrialEnabled.store(false,std::memory_order_release);
    NativeRowActivePhase.store(NativeRowPhase::Off,std::memory_order_release);
    NativeRowDeadline.store(0,std::memory_order_release);
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

    ReleaseHoverEventObserver();
    UiTextArmed.store(false,std::memory_order_release);
    HoverSpatialArmed.store(false,std::memory_order_release);
    TooltipProducerArmed.store(false,std::memory_order_release);
    TooltipRouteArmed.store(false,std::memory_order_release);
    HoverProbeArmed.store(false,std::memory_order_release);
    SoundArmed.store(false,std::memory_order_release);
    SoundLoaderBase.store(0,std::memory_order_release);
    ActiveGeometryMode.store(GeometryMode::Off, std::memory_order_release);
    BackgroundTintArmed.store(false,std::memory_order_release);
    BackgroundPaintObserveArmed.store(false,std::memory_order_release);
    GroundTextObserveArmed.store(false,std::memory_order_release);
    GroundTextCyanArmed.store(false,std::memory_order_release);
    CodeRenameArmed.store(false, std::memory_order_release);
    RenameArmed.store(false, std::memory_order_release);
    CodeBridgeArmed.store(false, std::memory_order_release);

    std::atomic_store_explicit(&PublishedFilterRules,
        std::shared_ptr<const FilterRuleTable>{},std::memory_order_release);
    FilterConfigPath.clear();
    HookInstalled.store(false, std::memory_order_release);
    CollectionHookInstalled.store(false, std::memory_order_release);
    OriginalCollectionHelper = nullptr;
    OriginalGetItemCode = nullptr;
    // The loader owns detour removal. Do not null out native trampolines
    // while other thread callbacks could still be forwarding through them.

    SoundThreads = nullptr;
    Context = nullptr;
    Base = 0;
    ImageSize = 0;
}

} // namespace SoE::LootFilter
