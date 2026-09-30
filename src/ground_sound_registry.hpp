#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace UnHoarder::SoundIdentity {

// A bounded, allocation-free, per-game registry of sound-eligible ground item
// unit IDs. Callers synchronize access; it never holds native item pointers.
// A successful inventory/cursor observation forgets precisely that live ID so
// a subsequent ground-label observation can claim a NEW sound ticket.
class Registry final {
public:
    static constexpr std::size_t SlotCount = 4096;
    static constexpr std::size_t CollisionScanLimit = 32;

    enum class Observation : std::uint8_t {
        New, AlreadySeen, Full, Invalid
    };

    Observation Observe(std::uint32_t unitId, std::uint32_t code,
                        std::uint64_t* ticket = nullptr) noexcept {
        if (ticket) *ticket = 0;
        if (unitId == 0 || code == 0) return Observation::Invalid;
        const auto first = static_cast<std::size_t>(unitId) % entries_.size();
        std::size_t firstTombstone = SlotCount;
        for (std::size_t i = 0; i < CollisionScanLimit; ++i) {
            const auto index = (first + i) % entries_.size();
            auto& entry = entries_[index];
            if (entry.unitId == unitId) {
                if (ticket) *ticket = entry.ticket;
                return Observation::AlreadySeen;
            }
            if (IsTombstone(entry)) {
                if (firstTombstone == SlotCount) firstTombstone = index;
                continue; // Never interrupt a collision chain on a deletion.
            }
            if (entry.unitId == 0) {
                auto& destination = entries_[firstTombstone == SlotCount
                                                ? index : firstTombstone];
                destination = {unitId, code, NextTicket()};
                if (ticket) *ticket = destination.ticket;
                return Observation::New;
            }
        }
        if (firstTombstone != SlotCount) {
            auto& destination = entries_[firstTombstone];
            destination = {unitId, code, NextTicket()};
            if (ticket) *ticket = destination.ticket;
            return Observation::New;
        }
        // Fail closed: a saturated registry must not replay every render frame.
        return Observation::Full;
    }

    bool Forget(std::uint32_t unitId) noexcept {
        if (unitId == 0) return false;
        const auto first = static_cast<std::size_t>(unitId) % entries_.size();
        for (std::size_t i = 0; i < CollisionScanLimit; ++i) {
            auto& entry = entries_[(first + i) % entries_.size()];
            if (entry.unitId == unitId) {
                entry = {0, TombstoneCode, 0};
                return true;
            }
            if (entry.unitId == 0 && !IsTombstone(entry)) return false;
        }
        return false;
    }

    // Queue-time sound ticket: picking up and re-dropping the same unit ID
    // invalidates audio queued before the pickup, even within one game epoch.
    bool IsCurrent(std::uint32_t unitId, std::uint64_t ticket) const noexcept {
        if (unitId == 0 || ticket == 0) return false;
        const auto first = static_cast<std::size_t>(unitId) % entries_.size();
        for (std::size_t i = 0; i < CollisionScanLimit; ++i) {
            const auto& entry = entries_[(first + i) % entries_.size()];
            if (entry.unitId == unitId) return entry.ticket == ticket;
            if (entry.unitId == 0 && !IsTombstone(entry)) return false;
        }
        return false;
    }

    void Clear() noexcept { entries_.fill({}); }

private:
    static constexpr std::uint32_t TombstoneCode = 1;
    struct Entry {
        std::uint32_t unitId{};
        std::uint32_t firstCode{};
        std::uint64_t ticket{};
    };
    static constexpr bool IsTombstone(const Entry& entry) noexcept {
        return entry.unitId == 0 && entry.firstCode == TombstoneCode;
    }
    std::uint64_t NextTicket() noexcept {
        ++nextTicket_;
        if (nextTicket_ == 0) ++nextTicket_;
        return nextTicket_;
    }
    std::array<Entry, SlotCount> entries_{};
    std::uint64_t nextTicket_{};
};

} // namespace UnHoarder::SoundIdentity
