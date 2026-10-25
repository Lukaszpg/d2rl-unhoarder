#include "src/maps_socket_memory.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
using namespace UnHoarder::MapsSocket;
alignas(4096) unsigned char dataPages[8192]{};
#pragma section(".proof",read,execute)
__declspec(allocate(".proof")) const unsigned char codePage[4096]={0x11,0x22};
unsigned checks{};
void Check(bool ok,const char* message) {
    ++checks;if(!ok){std::printf("FAIL %s\n",message);std::exit(1);}
}
int main() {
    ImageMemory image{};Check(image.Open(GetModuleHandleW(nullptr)),"fresh test image headers");
    for(unsigned i=0;i<sizeof(dataPages);++i)dataPages[i]=static_cast<unsigned char>(i);
    const auto base=reinterpret_cast<uintptr_t>(dataPages);
    std::array<unsigned char,8> a{},b{};
    auto queryCount=[] {return CurrentPageQuery::residentQueries.load()+CurrentPageQuery::fallbackQueries.load();};
    const auto before=queryCount();
    Check(ReadPair(image.Reader(),base+8,a.data(),a.size(),base+64,b.data(),b.size(),Kind::Data),"two live data witnesses");
    Check(queryCount()-before==1,"eligible same-page pair uses one fresh range proof");
    Check(a[0]==8 && b[0]==64,"both exact slices copied");
    dataPages[8]=99;
    Check(ReadPair(image.Reader(),base+8,a.data(),a.size(),base+64,b.data(),b.size(),Kind::Data) && a[0]==99,"next pair observes mutations");
    Check(ReadPair(image.Reader(),base+64,b.data(),b.size(),base+8,a.data(),a.size(),Kind::Data),"reverse address order");
    auto calls=queryCount();
    Check(ReadPair(image.Reader(),base+8,a.data(),a.size(),base+4096,b.data(),b.size(),Kind::Data),"cross-page narrow fallback");
    Check(queryCount()-calls==2,"cross-page pair retains two fresh observations");
    calls=queryCount();
    Check(ReadPair(image.Reader(),base+8,a.data(),a.size(),base+1024,b.data(),b.size(),Kind::Data),"wide same-page narrow fallback");
    Check(queryCount()-calls==2,"wide pair retains narrow reads");
    Check(!ReadPair(image.Reader(),base,a.data(),a.size(),UINTPTR_MAX-3,b.data(),b.size(),Kind::Data),"overflow refused");
    Check(!ReadPair(image.Reader(),base,a.data(),0,base+8,b.data(),b.size(),Kind::Data),"empty span refused");
    const auto code=reinterpret_cast<uintptr_t>(codePage);
    Check(ReadPair(image.Reader(),code,a.data(),a.size(),code+64,b.data(),b.size(),Kind::Code),"RX image witnesses");
    Check(a[0]==0x11 && a[1]==0x22,"code slices preserved");
    Check(!ReadPair(image.Reader(),code,a.data(),a.size(),code+64,b.data(),b.size(),Kind::Data),"code cannot supply mutable data");
    Check(!ReadPair(image.Reader(),base,a.data(),a.size(),base+64,b.data(),b.size(),Kind::Code),"data cannot supply code");
    ImageMemory wrong=image;wrong.base+=0x1000;
    Check(!ReadPair(wrong.Reader(),base,a.data(),a.size(),base+64,b.data(),b.size(),Kind::Data),"wrong allocation owner refused");
    DWORD original{};
    Check(VirtualProtect(dataPages,4096,PAGE_READONLY,&original)!=0,"set read-only data");
    Check(!ReadPair(image.Reader(),base,a.data(),a.size(),base+64,b.data(),b.size(),Kind::Data),"current write protection checked");
    DWORD previous{};Check(VirtualProtect(dataPages,4096,PAGE_NOACCESS,&previous)!=0,"set inaccessible data");
    Check(!ReadPair(image.Reader(),base,a.data(),a.size(),base+64,b.data(),b.size(),Kind::Data),"inaccessible page refused");
    Check(VirtualProtect(dataPages,4096,PAGE_READWRITE|PAGE_GUARD,&previous)!=0,"set guard page");
    Check(!ReadPair(image.Reader(),base,a.data(),a.size(),base+64,b.data(),b.size(),Kind::Data),"guard page refused");
    MEMORY_BASIC_INFORMATION page{};VirtualQuery(dataPages,&page,sizeof(page));
    Check((page.Protect&PAGE_GUARD)!=0,"guard flag not consumed");
    Check(VirtualProtect(dataPages,4096,original,&previous)!=0,"restore data protection");
    Check(ReadPair(image.Reader(),base+8,a.data(),a.size(),base+64,b.data(),b.size(),Kind::Data) && a[0]==99,"restored page freshly admitted");
    std::printf("%u live paired-reader checks passed\n",checks);
}
