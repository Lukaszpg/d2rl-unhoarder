#include "src/controller_ground_label.hpp"
#include "src/controller_ground_label_witnesses.hpp"
#include <array>
#include <cassert>
#include <vector>
#include <cstdio>
int main() {
    using namespace UnHoarder::ControllerGroundLabel;
    int unit{},otherUnit{}; std::array<unsigned char,0x148> label{},otherLabel{};
    Scope scope{&unit,label.data()+0x24,137};
    // Controller R8 is a separate geometry buffer and the native record ID
    // has not been published yet. Only this exact unit/output/ID scope owns it.
    assert(scope.Owns(&unit,label.data()+0x24,137));
    assert(!scope.Owns(&otherUnit,label.data()+0x24,137));
    assert(!scope.Owns(&unit,otherLabel.data()+0x24,137));
    assert(!scope.Owns(&unit,label.data()+0x28,137));
    for(auto id:{0u,136u,138u,UINT32_MAX}) assert(!scope.Owns(&unit,label.data()+0x24,id));
    assert(!Scope{}.Owns(&unit,label.data()+0x24,137));
    std::vector<unsigned char> image(0x1520000);
    struct Witness {std::uintptr_t rva;const unsigned char* data;std::size_t size;};
    const Witness witnesses[]={{0x151a263,Witness0,sizeof(Witness0)},
        {0x151a307,Witness1,sizeof(Witness1)},{0x1519c23,Witness2,sizeof(Witness2)},
        {0x1519e2e,Witness3,sizeof(Witness3)}};
    for(const auto& w:witnesses)std::memcpy(image.data()+w.rva,w.data,w.size);
    auto read=[&](std::uintptr_t rva,void* out,std::size_t size){
        if(rva>image.size() || size>image.size()-rva)return false;
        std::memcpy(out,image.data()+rva,size);return true;
    };
    assert(Verify(read));
    for(const auto& w:witnesses)for(std::size_t i=0;i<w.size;++i){
        image[w.rva+i]^=1;assert(!Verify(read));image[w.rva+i]^=1;
    }
    assert(!Verify([](std::uintptr_t,void*,std::size_t){return false;}));
    std::puts("Exact controller ABI witnesses and unit/output/ID ownership pass; every witness-byte mutation and stale or unrelated scope refuse.");
}
