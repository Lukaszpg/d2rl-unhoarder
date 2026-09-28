#pragma once
#include <cstdint>
// A reused row address is not an ownership identifier. Sequence numbers are
// assigned after a completed append has been recorded in the phase bucket.
namespace NativeRowLatestMatch {
constexpr bool Precedes(std::int64_t appendQpc,
    std::int64_t renderQpc) noexcept {
    return appendQpc>0 && renderQpc>=appendQpc;
}
constexpr bool FenceAllows(std::uint64_t appendSeq,
    std::uint64_t rendererFence) noexcept {
    return appendSeq!=0 && appendSeq<=rendererFence;
}
constexpr bool Later(std::uint64_t nextSequence,std::int64_t nextQpc,
    std::uint64_t currentSequence,std::int64_t currentQpc) noexcept {
    return nextSequence>currentSequence ||
        (nextSequence==currentSequence && nextQpc>currentQpc);
}
}
