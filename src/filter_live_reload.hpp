#pragma once
// Portable file-stability state machine. Filesystem I/O belongs to the
// background worker, not this type or any native D2R item/rendering hook.
#include <cstdint>
#include <filesystem>

namespace SoE::LootFilter::FilterLiveReload {
struct Stamp {
    bool present{};
    std::uint64_t bytes{};
    std::filesystem::file_time_type modified{};
    bool operator==(const Stamp&) const noexcept = default;
};

class Watcher final {
public:
    // First observation is a baseline, NOT a reload. Every subsequent distinct
    // stamp produces at most one request after an unchanged settling interval.
    // A refused invalid edit is only retried if the file changes again or on
    // explicit manual reload, preventing warning floods on a broken JSON file.
    bool Observe(const Stamp& stamp,std::uint64_t now,
                 std::uint64_t settleMs) noexcept {
        if (!seeded_) {Resync(stamp);return false;}
        if (!(stamp==pending_)) {
            pending_=stamp;
            lastChanged_=now;
            unsettled_=true;
            return false;
        }
        if (!unsettled_ || now<lastChanged_ ||
            now-lastChanged_<settleMs) return false;
        unsettled_=false;
        return true;
    }
    void Resync(const Stamp& stamp) noexcept {
        pending_=stamp;
        lastChanged_=0;
        seeded_=true;
        unsettled_=false;
    }
private:
    Stamp pending_{};
    std::uint64_t lastChanged_{};
    bool seeded_{},unsettled_{};
};
} // namespace SoE::LootFilter::FilterLiveReload
