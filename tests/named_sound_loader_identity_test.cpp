#include "src/named_sound_loader_identity.hpp"
#include <cassert>
#include <string_view>
int main() {
    using SoundLoaderIdentity::Classify;
    using SoundLoaderIdentity::Layout;
    assert(Classify(0x6AAFC972U,0x5602000U)==Layout::Loader130);
    assert(Classify(0x6AB3782CU,0x5643000U)==Layout::Loader131);
    assert(Classify(0x6AAFC972U,0x5643000U)==Layout::Unknown);
    assert(Classify(0x6AB3782CU,0x5602000U)==Layout::Unknown);
    assert(Classify(0,0)==Layout::Unknown);
    assert(std::string_view(SoundLoaderIdentity::Name(Layout::Loader131))=="loader-1.3.1");
}
