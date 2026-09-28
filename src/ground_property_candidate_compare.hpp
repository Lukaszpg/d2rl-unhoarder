#pragma once
// Portable, read-only interpretation of two CANDIDATE fields observed in the
// 0.2.38 ground capture. These are evidence only; they are NOT a qualified
// production ItemData layout, and MUST NOT feed filter rule decisions.
// The raw item-flags dword is retained without attributing any flag meaning.
#include <cstddef>
#include <cstdint>

namespace SoE::LootFilter::GroundCandidateProbe {
constexpr std::size_t QualityCandidateOffset=0x00;
constexpr std::size_t RawFlagsCandidateOffset=0x18;
constexpr std::size_t LevelCandidateOffset=0x38;
constexpr std::size_t MinimumCandidateBytes=LevelCandidateOffset+sizeof(std::uint32_t);

struct Snapshot {
    bool available{};
    std::uint32_t qualityCandidate{};
    std::uint32_t levelCandidate{};
    std::uint32_t rawFlagsCandidate{};
};

constexpr std::uint32_t ReadLe32(const std::uint8_t* bytes) noexcept {
    return std::uint32_t(bytes[0]) |
        (std::uint32_t(bytes[1])<<8) |
        (std::uint32_t(bytes[2])<<16) |
        (std::uint32_t(bytes[3])<<24);
}

inline Snapshot Decode(const std::uint8_t* bytes,
    std::size_t bytesRead) noexcept {
    Snapshot snapshot{};
    if(!bytes || bytesRead<MinimumCandidateBytes) return snapshot;
    snapshot.available=true;
    snapshot.qualityCandidate=ReadLe32(bytes+QualityCandidateOffset);
    snapshot.levelCandidate=ReadLe32(bytes+LevelCandidateOffset);
    snapshot.rawFlagsCandidate=ReadLe32(bytes+RawFlagsCandidateOffset);
    return snapshot;
}

struct Comparison {
    bool available{};
    bool qualityEqualsSdk{};
    bool levelEqualsSdk{};
};
constexpr Comparison Compare(Snapshot sample,std::uint32_t sdkQuality,
    std::uint32_t sdkLevel) noexcept {
    return {sample.available,
        sample.available && sample.qualityCandidate==sdkQuality,
        sample.available && sample.levelCandidate==sdkLevel};
}
} // namespace SoE::LootFilter::GroundCandidateProbe
