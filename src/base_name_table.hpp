#pragma once
// Reload-time lookup of the literal `name` column in Weapons.txt / Armor.txt.
// Resolve to base item codes, NOT numeric item class IDs or localized UI text.
// No file access is performed on the ground-label / sound / pickup hot paths.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SoE::LootFilter::BaseNameTable {

inline std::uint32_t PackCode(std::string_view text) noexcept {
    std::uint32_t result{};
    if (text.empty() || text.size()>4) return 0;
    for(std::size_t i=0;i<text.size();++i) {
        const auto c=static_cast<unsigned char>(text[i]);
        if(c<0x21 || c>0x7e) {
            if(c!=' ' || i<3) return 0;
            // Space is only acceptable as final padding in a 4-byte code.
            if(i+1!=text.size()) return 0;
        }
        result|=static_cast<std::uint32_t>(c)<<(8U*i);
    }
    // Runtime item codes are canonicalized by removing trailing space/NUL.
    for(int i=3;i>=0;--i) {
        const auto c=static_cast<unsigned char>(result>>(8U*i));
        if(c && c!=' ') break;
        result&=~(0xffU<<(8U*i));
    }
    return result;
}

struct Catalog {
    std::unordered_map<std::string,std::vector<std::uint32_t>> byName;
    std::size_t weaponRows{},armorRows{};

    const std::vector<std::uint32_t>* Find(std::string_view name) const {
        const auto it=byName.find(std::string(name));
        return it==byName.end()?nullptr:&it->second;
    }
};

inline bool ReadTable(const std::filesystem::path& path,Catalog& out,
    std::size_t& rowCount,std::string& error) {
    std::error_code ec;
    const auto bytes=std::filesystem::file_size(path,ec);
    if(ec || bytes==0 || bytes>8U*1024U*1024U) {
        error="baseName-table-missing-empty-or-over-8MiB";return false;
    }
    std::ifstream file(path,std::ios::binary);
    if(!file) {error="baseName-table-open-failed";return false;}
    std::string line;
    if(!std::getline(file,line)) {
        error="baseName-table-missing-header";return false;
    }
    if(line.size()>=3 && line.compare(0,3,"\xef\xbb\xbf")==0)
        line.erase(0,3);
    if(!line.empty() && line.back()=='\r') line.pop_back();
    // Keep empty TSV cells, otherwise column positions shift.
    const auto fields=[](std::string_view row) {
        std::vector<std::string_view> result;
        for(std::size_t pos=0;pos<=row.size();) {
            const auto end=row.find('\t',pos);
            if(end==std::string_view::npos) {
                result.push_back(row.substr(pos));break;
            }
            result.push_back(row.substr(pos,end-pos));pos=end+1;
        }
        return result;
    };
    const auto headings=fields(line);
    std::size_t nameIndex=headings.size(),codeIndex=headings.size();
    for(std::size_t i=0;i<headings.size();++i) {
        if(headings[i]=="name") nameIndex=i;
        if(headings[i]=="code") codeIndex=i;
    }
    if(nameIndex==headings.size() || codeIndex==headings.size()) {
        error="baseName-table-requires-literal-name-and-code-columns";
        return false;
    }
    while(std::getline(file,line)) {
        if(line.size()>65536) {error="baseName-table-row-over-64KiB";return false;}
        if(!line.empty() && line.back()=='\r') line.pop_back();
        if(line.empty()) continue;
        const auto row=fields(line);
        if(nameIndex>=row.size() || codeIndex>=row.size()) {
            error="baseName-table-short-row";return false;
        }
        const auto name=row[nameIndex],code=row[codeIndex];
        if(name.empty() || code.empty()) continue;
        if(name.size()>127 || name.find('\r')!=name.npos ||
           name.find('\n')!=name.npos) {
            error="baseName-table-invalid-name";return false;
        }
        const auto packed=PackCode(code);
        if(!packed) {error="baseName-table-invalid-item-code";return false;}
        out.byName[std::string(name)].push_back(packed);
        if(++rowCount>4096) {
            error="baseName-table-over-4096-named-rows";return false;
        }
    }
    if(file.bad() || rowCount==0) {
        error="baseName-table-read-failed-or-empty";return false;
    }
    return true;
}

// Prefer the exact mod's extracted MPQ data, never silently select a random
// MPQ when several candidates contain different tables. Same-folder copies
// alongside the JSON are supported for standalone deployments.
inline std::filesystem::path DiscoverExcel(const std::filesystem::path& config) {
    namespace fs=std::filesystem;
    const auto hasPair=[](const fs::path& dir) {
        std::error_code ec;
        const bool w=fs::is_regular_file(dir/L"weapons.txt",ec);
        if(ec || !w) return false;
        ec.clear();
        return fs::is_regular_file(dir/L"armor.txt",ec) && !ec;
    };
    for(auto root=config.parent_path();!root.empty();) {
        if(hasPair(root)) return root;
        if(hasPair(root/L"data"/L"global"/L"excel"))
            return root/L"data"/L"global"/L"excel";
        const auto ownMpq=root/(root.filename().wstring()+L".mpq")/
            L"data"/L"global"/L"excel";
        if(hasPair(ownMpq)) return ownMpq;
        std::vector<fs::path> candidates;
        std::error_code ec;
        for(fs::directory_iterator it(root,ec),end;!ec && it!=end;
            it.increment(ec)) {
            if(!it->is_directory(ec)) {if(ec) break;continue;}
            if(it->path().extension()!=L".mpq") continue;
            const auto excel=it->path()/L"data"/L"global"/L"excel";
            if(hasPair(excel)) candidates.push_back(excel);
        }
        if(candidates.size()==1) return candidates.front();
        if(candidates.size()>1) return {}; // ambiguous mod tables: fail closed at reload
        const auto parent=root.parent_path();
        if(parent==root) break;
        root=parent;
    }
    return {};
}

inline bool LoadPair(const std::filesystem::path& excel,
    Catalog& out,std::string& error) {
    Catalog staged{};
    if(!ReadTable(excel/L"weapons.txt",staged,staged.weaponRows,error) ||
       !ReadTable(excel/L"armor.txt",staged,staged.armorRows,error)) return false;
    for(auto& [name,codes]:staged.byName) {
        std::sort(codes.begin(),codes.end());
        codes.erase(std::unique(codes.begin(),codes.end()),codes.end());
    }
    out=std::move(staged);
    return true;
}

} // namespace SoE::LootFilter::BaseNameTable
