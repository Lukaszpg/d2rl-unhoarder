#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstring>
namespace UnHoarder::MapsSocket {
inline bool VerifyFileHash(HMODULE module, const unsigned char (&expected)[32], unsigned char* actual=nullptr, bool* readable=nullptr) noexcept {
    if(readable)*readable=false;
    if (!module) return false;
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(module,path,32768)) return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm=nullptr; BCRYPT_HASH_HANDLE hash=nullptr;
    unsigned char digest[32]{}; unsigned char buffer[65536]; DWORD got=0;
    bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(ok) ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    while(ok) {
        if(!ReadFile(file,buffer,sizeof(buffer),&got,nullptr)) {ok=false;break;}
        if(!got) break;
        ok=BCryptHashData(hash,buffer,got,0)>=0;
    }
    if(ok) ok=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
    if(hash) BCryptDestroyHash(hash);
    if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    CloseHandle(file);
    if(ok && actual)std::memcpy(actual,digest,32);
    if(readable)*readable=ok;
    return ok && std::memcmp(digest,expected,32)==0;
}
}
