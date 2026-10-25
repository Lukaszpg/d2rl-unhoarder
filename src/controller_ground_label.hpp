#pragma once
#include <cstdint>
#include <cstddef>
namespace UnHoarder::ControllerGroundLabel {
inline constexpr std::uintptr_t FormatterReturn=0x151a2b2, PaintReturn=0x1519e46;
struct Scope {
    const void* unit{}; const void* output{}; std::uint32_t id{};
    bool Owns(const void* candidateUnit,const void* candidateOutput,std::uint32_t candidateId) const noexcept {
        return unit && output && id && id!=UINT32_MAX && unit==candidateUnit &&
            output==candidateOutput && id==candidateId;
    }
};
}
