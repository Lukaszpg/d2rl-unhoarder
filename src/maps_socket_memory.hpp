#pragma once
#include "maps_socket_contract.hpp"
#include <windows.h>
#include "guarded_local_read.hpp"
#include "current_page_query.hpp"

namespace UnHoarder::MapsSocket {
// Only readable executable image sections belonging to this loaded companion
// supply code. Its object/cookie must live in writable, non-executable image
// data. No scan, guessed RVA, executable-only ownership test or cached proof.
struct ImageMemory {
    uintptr_t base{};
    uint32_t size{};
    IMAGE_SECTION_HEADER sections[96]{};
    unsigned count{};
    static bool Copy(uintptr_t address,void* output,size_t bytes) noexcept {
        SIZE_T got{};
        return address && bytes && address<=UINTPTR_MAX-bytes &&
            ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),output,bytes,&got) && got==bytes;
    }
    bool Open(HMODULE module) noexcept {
        base=reinterpret_cast<uintptr_t>(module);
        // Fresh metadata per guard. Combine the usual PE header reads without
        // retaining any page, section, byte, ownership or forwarding proof.
        // Unusual layouts and inaccessible optional tails retain narrow reads.
        unsigned char headerBytes[0x1000];
        const bool headerReady=Copy(base,headerBytes,sizeof(headerBytes));
        const auto headerCopy=[&](uintptr_t at,void* out,size_t bytes) noexcept {
            if(headerReady && at>=base && at-base<=sizeof(headerBytes) && bytes<=sizeof(headerBytes)-(at-base)) {
                std::memcpy(out,headerBytes+(at-base),bytes);return true;
            }
            return Copy(at,out,bytes);
        };
        IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
        if(!headerCopy(base,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x1000 ||
           base>UINTPTR_MAX-static_cast<uintptr_t>(dos.e_lfanew))return false;
        const auto header=base+static_cast<uintptr_t>(dos.e_lfanew);
        if(!headerCopy(header,&nt,sizeof(nt)) || nt.Signature!=IMAGE_NT_SIGNATURE ||
           nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
           nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64) ||
           !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections>std::size(sections) ||
           nt.OptionalHeader.SizeOfImage<0x1000 || nt.OptionalHeader.SizeOfImage>0x40000000)return false;
        size=nt.OptionalHeader.SizeOfImage;count=nt.FileHeader.NumberOfSections;
        const auto table=header+sizeof(nt);
        if(base>UINTPTR_MAX-size || table<base || table-base>size || count*sizeof(sections[0])>size-(table-base) ||
           !headerCopy(table,sections,count*sizeof(sections[0])))return false;
        for(unsigned i=0;i<count;++i) {
            const auto& section=sections[i];
            if(section.VirtualAddress>size || section.Misc.VirtualSize>size-section.VirtualAddress)return false;
            for(unsigned j=0;j<i;++j) {
                const auto& previous=sections[j];
                if(section.VirtualAddress<previous.VirtualAddress+previous.Misc.VirtualSize &&
                   previous.VirtualAddress<section.VirtualAddress+section.Misc.VirtualSize)return false;
            }
        }
        return true;
    }
    bool Read(uintptr_t address,void* output,size_t bytes,Kind kind) const noexcept {
        if(!bytes || address>UINTPTR_MAX-bytes)return false;
        if(kind!=Kind::Trampoline) {
            if(address<base || address-base>size || bytes>size-(address-base))return false;
            bool admitted=false;
            for(unsigned i=0;i<count;++i) {
                const auto& s=sections[i];const auto rva=address-base;
                if(rva<s.VirtualAddress || rva-s.VirtualAddress>s.Misc.VirtualSize || bytes>s.Misc.VirtualSize-(rva-s.VirtualAddress))continue;
                const auto flags=s.Characteristics;
                admitted=(flags&IMAGE_SCN_MEM_READ) && !(flags&IMAGE_SCN_MEM_DISCARDABLE) &&
                    (kind==Kind::Code?((flags&IMAGE_SCN_MEM_EXECUTE) && !(flags&IMAGE_SCN_MEM_WRITE)):
                                     ((flags&IMAGE_SCN_MEM_WRITE) && !(flags&IMAGE_SCN_MEM_EXECUTE)));
            }
            if(!admitted)return false;
        }
        DWORD currentProtection{};
        if(CurrentPageQuery::SinglePage(address,bytes,
                kind==Kind::Trampoline?0:base,currentProtection)) {
            if(currentProtection&(PAGE_GUARD|PAGE_NOACCESS))return false;
            const bool executable=(currentProtection&(PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
            const bool writable=(currentProtection&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
            if(kind==Kind::Data?(executable || !writable):!executable)return false;
            if(kind==Kind::Code && (currentProtection&(PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
            return GuardedLocalRead::Detail::CopyReadable(reinterpret_cast<const void*>(address),output,bytes);
        }
        for(auto cursor=address;cursor<address+bytes;) {
            MEMORY_BASIC_INFORMATION region{};
            if(!VirtualQuery(reinterpret_cast<void*>(cursor),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
               (region.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
            const bool executable=(region.Protect&(PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
            const bool writable=(region.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
            if(kind==Kind::Data?(executable || !writable):!executable)return false;
            if(kind!=Kind::Trampoline && (region.Type!=MEM_IMAGE || reinterpret_cast<uintptr_t>(region.AllocationBase)!=base))return false;
            if(kind==Kind::Code && (region.Protect&(PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
            const auto start=reinterpret_cast<uintptr_t>(region.BaseAddress);
            if(region.RegionSize>UINTPTR_MAX-start || start+region.RegionSize<=cursor)return false;
            cursor=start+region.RegionSize;
        }
        // The unchanged live kind/page/ownership checks above qualify every
        // region. The copy still refuses a subsequent fault or decommit race.
        return GuardedLocalRead::Detail::CopyReadable(reinterpret_cast<const void*>(address),output,bytes);
    }
    bool ReadPair(uintptr_t first,void* a,size_t aSize,uintptr_t second,void* b,size_t bSize,Kind kind) const noexcept {
        if(!first || !second || !a || !b || !aSize || !bSize ||
           first>UINTPTR_MAX-aSize || second>UINTPTR_MAX-bSize)return false;
        const auto start=first<second?first:second;
        const auto end=first+aSize>second+bSize?first+aSize:second+bSize;
        const auto pageSize=CurrentPageQuery::SystemApi().pageSize;
        unsigned char snapshot[512];
        // One fresh proof for a covering range also proves both slices. Keep
        // unusual sections/pages on the original narrow path; retain nothing
        // across calls, guards, or the native property invocation.
        if(end-start<=sizeof(snapshot) && pageSize && start/pageSize==(end-1)/pageSize &&
           Read(start,snapshot,end-start,kind)) {
            std::memcpy(a,snapshot+(first-start),aSize);
            std::memcpy(b,snapshot+(second-start),bSize);
            return true;
        }
        return Read(first,a,aSize,kind) && Read(second,b,bSize,kind);
    }
    Memory Reader() noexcept {
        return {this,[](void* user,uintptr_t address,void* output,size_t bytes,Kind kind) noexcept {
            return static_cast<ImageMemory*>(user)->Read(address,output,bytes,kind);
        },[](void* user,uintptr_t first,void* a,size_t aSize,uintptr_t second,void* b,size_t bSize,Kind kind) noexcept {
            return static_cast<ImageMemory*>(user)->ReadPair(first,a,aSize,second,b,bSize,kind);
        }};
    }
};
} // namespace UnHoarder::MapsSocket
