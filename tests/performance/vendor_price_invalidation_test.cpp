#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>

unsigned inspected{},checks{};
struct CountedValidity {
    bool value{};
    operator bool() const noexcept {++inspected;return value;}
    CountedValidity& operator=(bool next) noexcept {value=next;return *this;}
};
struct CacheEntry {std::uint32_t unitId{};CountedValidity valid{};};
std::array<CacheEntry,512> VendorSellPriceCache{};
std::mutex VendorSellPriceCacheMutex;
// The exact production invalidator runs against an instrumented bool with
// identical truth values. Count inspected entries without timing assertions.
#include "vendor_price_invalidation.inc"
void Check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
int main() {
    for(unsigned id:{1u,17u,511u,512u,1025u,UINT32_MAX}) {
        VendorSellPriceCache={};const auto slot=id%VendorSellPriceCache.size();
        VendorSellPriceCache[slot].unitId=id;VendorSellPriceCache[slot].valid=true;
        inspected=0;ForgetVendorSellPriceItem(id);
        Check(!VendorSellPriceCache[slot].valid.value,"matching entry invalidated");
        Check(inspected==1,"direct-mapped invalidation inspects exactly one entry");
        const auto collision=id==UINT32_MAX?id-512:id+512;
        VendorSellPriceCache[slot].unitId=collision;VendorSellPriceCache[slot].valid=true;
        inspected=0;ForgetVendorSellPriceItem(id);
        Check(VendorSellPriceCache[slot].valid.value && VendorSellPriceCache[slot].unitId==collision,"colliding identity is preserved");
        Check(inspected==1,"collision remains constant work");
    }
    inspected=0;ForgetVendorSellPriceItem(0);Check(inspected==0,"zero identity does no cache work");
    std::printf("%u production price-cache invalidation checks passed.\n",checks);
}
