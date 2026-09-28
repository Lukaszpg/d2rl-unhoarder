#pragma once
#include <cstdint>
namespace NativeRowBgLivePolicy {
constexpr bool RecentChain(std::int64_t earlier,std::int64_t later,
    std::int64_t frequency) noexcept {
    return frequency>0 && earlier>0 && later>=earlier &&
        later-earlier<=frequency/500; // 2ms fail-closed
}
constexpr bool SameLabel(std::uint64_t appendEpoch,
    std::uint64_t activeEpoch,std::uint64_t appendedLabelSeq,
    std::uint64_t currentLabelSeq,std::uint32_t appendedId,
    std::uint32_t currentId,std::uint32_t appendedClass,
    std::uint32_t currentClass,std::uint32_t appendedCode,
    std::uint32_t currentCode) noexcept {
    return appendEpoch!=0 && appendEpoch==activeEpoch &&
        appendedLabelSeq!=0 && appendedLabelSeq==currentLabelSeq &&
        appendedId!=0 && appendedId==currentId &&
        appendedClass==currentClass && appendedCode!=0 &&
        appendedCode==currentCode;
}
}
