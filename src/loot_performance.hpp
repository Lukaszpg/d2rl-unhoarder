#pragma once
#include "perf_stats.hpp"
#include "current_page_query.hpp"
#include <D2RLPlugin/api.h>
namespace UnHoarder::Perf {
enum Metric : std::size_t {PollWake,PollSkipped,PollPending,PollDispatchFailure,
    Poll,Enumeration,Name,Identity,Visibility,Rules,MinimapRender,Properties,Sockets,Style,Glyph,LabelPreflight,LabelNoHide,Quality,MinimapPosition,MinimapPositionSkipped,FormatterObservation,FormatterReuse,FormatterFallback,Pickup,PickupBlocked,Count};
inline std::array<LootPerf::Counter,Count> Counters{};
inline LootPerf::ReportWindow<Count> Window;
using Scope=LootPerf::Scope;
inline constexpr std::array<LootPerf::Description,Count> Names{{
    {"poll-wakeup","items"},{"poll-no-demand-or-service","items"},
    {"poll-already-pending","items"},{"poll-dispatch-failure","items"},
    {"inventory-poll","carriedItems"},{"sdk-enumeration","carriedItems"},
    {"ground-name","items"},{"ground-identity","items"},
    {"visibility-service","items"},{"filter-rules","rulesAvailable"},
    {"minimap-render","frames"},{"native-properties","items"},
    {"socket-observation","items"},{"ground-style","labels"},{"glyph-forward","glyphs"},
    {"label-visibility-preflight","items"},{"label-no-hide-proven","items"},
    {"native-quality-observation","items"},{"minimap-position","items"},
    {"minimap-position-no-icon","items"},{"formatter-observation-published","items"},
    {"formatter-observation-reused","items"},{"formatter-observation-fallback","items"},
    {"pickup-guard","requests"},{"pickup-hidden-blocked","requests"}}};
inline void Report(const D2RL::PluginContext* context) noexcept {
    if(!context)return;
    const auto emit=[&](const char* line) noexcept {
        context->LogInfo(line);
        context->WriteConsoleMessage(line,D2RL::ConsoleMessageKind::Output);
    };
    Window.Print("LOOT_PERF",UNHOARDER_VERSION_STRING,Names,Counters,emit);
    CurrentPageQuery::Report("unhoarder",emit);
}
}
