#include "../src/filter_live_reload.hpp"
#include <cassert>
using namespace SoE::LootFilter::FilterLiveReload;
int main() {
    Watcher watcher;
    Stamp v1{true,100,std::filesystem::file_time_type(std::filesystem::file_time_type::duration(100))};
    Stamp v2{true,101,std::filesystem::file_time_type(std::filesystem::file_time_type::duration(101))};
    Stamp v3{true,101,std::filesystem::file_time_type(std::filesystem::file_time_type::duration(102))};
    Stamp absent{};
    assert(!watcher.Observe(v1,1000,650)); // initial baseline
    assert(!watcher.Observe(v1,2000,650));
    assert(!watcher.Observe(v2,2200,650));
    assert(!watcher.Observe(v2,2849,650));
    assert(watcher.Observe(v2,2850,650));
    assert(!watcher.Observe(v2,4000,650)); // one attempt only
    assert(!watcher.Observe(v3,4100,650)); // same length, newer mtime
    assert(!watcher.Observe(v1,4300,650)); // save again resets debounce
    assert(!watcher.Observe(v1,4949,650));
    assert(watcher.Observe(v1,4950,650));
    watcher.Resync(v3); // F9 reload avoids immediate duplicate auto reload
    assert(!watcher.Observe(v3,6000,650));
    assert(!watcher.Observe(absent,6100,650));
    assert(watcher.Observe(absent,6750,650));
    assert(!watcher.Observe(absent,8000,650));
    assert(!watcher.Observe(v1,8200,650));
    assert(watcher.Observe(v1,8850,650));
}
