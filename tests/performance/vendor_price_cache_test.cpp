#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

constexpr std::size_t VendorSellPriceCacheCapacity=512;
constexpr ULONGLONG VendorSellPriceCacheTtlMs=1000;
ULONGLONG fixtureTick=1000;
ULONGLONG Clock() noexcept {return fixtureTick;}
#include "vendor_price_entries.inc"
#define GetTickCount64 Clock
#include "vendor_price_cache.inc"
#undef GetTickCount64
unsigned checks{};
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
int main() {
    unsigned char unit{},other{};std::uint32_t price{};ULONGLONG observed{};
    auto get=[&](std::uint64_t generation) {return TryGetCachedVendorSellPrice(17,22,33,&unit,generation,price,observed);};
    StoreVendorSellPrice(17,22,33,&unit,100,fixtureTick,1);
    Check(get(1) && price==100 && observed==1000,"same generation returns cached native value");
    Check(!get(2),"re-enabling price rules cannot reuse a value from an inactive generation");
    StoreVendorSellPrice(17,22,33,&unit,200,fixtureTick,2);
    Check(!get(1) && get(2) && price==200,"a previous-generation in-flight read cannot satisfy the new rules");
    Check(!TryGetCachedVendorSellPrice(17,22,33,&other,2,price,observed),"borrowed native identity still required");
    Check(!TryGetCachedVendorSellPrice(17,23,33,&unit,2,price,observed),"class identity still required");
    Check(!TryGetCachedVendorSellPrice(17,22,34,&unit,2,price,observed),"code identity still required");
    fixtureTick=2000;Check(get(2),"existing inclusive TTL endpoint retained");
    fixtureTick=2001;Check(!get(2),"expired native value remains unavailable");
    fixtureTick=999;Check(!get(2),"rewound observation time remains unavailable");
    fixtureTick=1000;StoreVendorSellPrice(529,22,33,&unit,300,fixtureTick,2);
    Check(!get(2),"colliding item replaces its direct slot without aliasing identities");
    VendorSellPriceCache.fill({});
    Check(!TryGetCachedVendorSellPrice(529,22,33,&unit,2,price,observed),"session reset clears cached values");
    std::printf("%u production price-cache identity, generation and TTL checks passed.\n",checks);
}
