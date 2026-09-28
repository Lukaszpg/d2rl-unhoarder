#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Pure policy: an exact captured Win64 return-address witness proves ONLY
// that the formatter's caller was still on the callback stack, not that its
// function initially hit-tested the mouse or rendered the ground model.
namespace WorldProbeLabelStack {
inline constexpr std::array<std::uintptr_t,3> KnownReturnRvas{{
    0x880AAA,0x880AE8,0x880B11
}};
struct Witness final {
    std::uintptr_t returnRva{};
    unsigned frameIndex{};
    unsigned distinctSiteCount{};
};
inline Witness Identify(std::span<void* const> frames,
                       std::uintptr_t imageBase,
                       std::uintptr_t imageSize) noexcept {
    Witness result{};
    if(!imageBase || !imageSize)return result;
    for(std::size_t i=0;i<frames.size();++i){
        const auto address=reinterpret_cast<std::uintptr_t>(frames[i]);
        if(address<imageBase || address-imageBase>=imageSize)continue;
        const auto returnRva=address-imageBase;
        for(const auto known:KnownReturnRvas){
            if(returnRva!=known)continue;
            if(!result.distinctSiteCount){
                result.returnRva=known;
                result.frameIndex=static_cast<unsigned>(i);
                ++result.distinctSiteCount;
            }else if(result.returnRva!=known){
                // Even an unusual, nested label call must not be attributed
                // exclusively to the first matching formatter callsite.
                ++result.distinctSiteCount;
            }
            break;
        }
    }
    return result;
}
} // namespace WorldProbeLabelStack
