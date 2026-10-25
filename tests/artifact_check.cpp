#include <D2RLPlugin/api.h>
#include <D2RLPlugin/resource.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
int main(int argc,char** argv){
    if(argc!=3)return 1;
    auto resource=LoadLibraryExA(argv[1],nullptr,LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if(!resource)return 2;
    auto manifest=FindResourceA(resource,MAKEINTRESOURCEA(D2RL_PLUGIN_MANIFEST_RESOURCE_ID),MAKEINTRESOURCEA(10));
    if(!manifest || SizeofResource(resource,manifest)!=sizeof(DWORD))return 3;
    auto abi=static_cast<const DWORD*>(LockResource(LoadResource(resource,manifest)));
    if(!abi || *abi!=D2RL_PLUGIN_ABI_VERSION)return 4;
    FreeLibrary(resource);
    auto module=LoadLibraryExA(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!module)return 5;
    auto getInfo=reinterpret_cast<D2RL::GetPluginInfoFn>(GetProcAddress(module,"D2RLoaderGetPluginInfo"));
    if(!getInfo || !GetProcAddress(module,"D2RLoaderLoadPlugin") || !GetProcAddress(module,"D2RLoaderUnloadPlugin"))return 6;
    const auto* info=getInfo();
    if(!info || info->infoSize!=D2RL::PluginInfoSize || info->abiVersion!=D2RL_PLUGIN_ABI_VERSION)return 7;
    if(std::strcmp(info->id,"unhoarder") || std::strcmp(info->name,"UnHoarder") || std::strcmp(info->version,argv[2]))return 8;
    if(info->flags!=(D2RL::PluginFlags::Client|D2RL::PluginFlags::NativeHooks|D2RL::PluginFlags::ModScopedOnly))return 9;
    FreeLibrary(module);
    std::puts("UnHoarder resource ABI, exports, metadata, version, role and dependency loading passed.");
}
