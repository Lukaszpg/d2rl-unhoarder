#include "interop/unhoarder_tooltip_compat_v1.hpp"
#include <cassert>
#include <cstdint>

int main() {
    namespace C = UnHoarder::TooltipCompatV1;
    static_assert(C::ServiceVersion == 1);
    static_assert(C::SharedLabelPaintRva == 0x001FA8E0ULL);
    static_assert(C::GlyphRendererRva == 0x00658510ULL);
    static_assert(C::ServiceSize == sizeof(C::Service));
    static_assert(C::ServiceRequiredSize == C::ServiceSize);
    assert(C::InvalidRegistrationHandle == 0);
    assert(C::MaxSubscribers == 16);
    return 0;
}
