#pragma once
// Compile literal ItemTypes.txt ItemType/Code selectors into concrete item
// codes at JSON reload. Expand Equiv1/Equiv2 transitively and honor each
// item's type/type2 from weapons.txt, armor.txt and misc.txt. This is NOT
// a native D2R item reader: no hook-time filesystem access or allocations.
#include "base_name_table.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SoE::LootFilter::ItemTypeTable {
namespace fs=std::filesystem;

struct TypeRow {
    std::string parent1,parent2;
};
struct Catalog {
    std::unordered_map<std::string,TypeRow> types; // ItemTypes.Code
    std::unordered_map<std::string,std::vector<std::string>> names; // literal ItemTypes.ItemType
    std::unordered_map<std::string,std::vector<std::uint32_t>> itemCodesByType;
    std::size_t typeRows{},weaponRows{},armorRows{},miscRows{};
    const std::vector<std::uint32_t>* Find(std::string_view typeOrName) const {
        const auto it=itemCodesByType.find(std::string(typeOrName));
        return it==itemCodesByType.end()?nullptr:&it->second;
    }
};
inline std::vector<std::string_view> SplitTsv(std::string_view row) {
    std::vector<std::string_view> cells;
    for(std::size_t pos=0;pos<=row.size();) {
        const auto end=row.find('\t',pos);
        if(end==std::string_view::npos) {cells.push_back(row.substr(pos));break;}
        cells.push_back(row.substr(pos,end-pos));pos=end+1;
    }
    return cells;
}
inline std::string TypeCode(std::string_view cell) {
    // Excel may pad an ASCII type code to four bytes. Names are NOT trimmed.
    while(!cell.empty() && cell.back()==' ') cell.remove_suffix(1);
    if(!BaseNameTable::PackCode(cell))return {};
    return std::string(cell);
}
inline bool ReadRows(const fs::path& path,std::vector<std::string>& lines,
    std::vector<std::string_view>& headings,std::string& error) {
    std::error_code ec;
    const auto bytes=fs::file_size(path,ec);
    if(ec || !bytes || bytes>32U*1024U*1024U) {
        error="itemType-table-missing-empty-or-over-32MiB:"+path.filename().string();
        return false;
    }
    std::ifstream file(path,std::ios::binary);
    if(!file) {error="itemType-table-open-failed";return false;}
    std::string line;
    if(!std::getline(file,line)) {error="itemType-table-missing-header";return false;}
    if(line.compare(0,3,"\xef\xbb\xbf")==0)line.erase(0,3);
    if(!line.empty() && line.back()=='\r')line.pop_back();
    lines.push_back(std::move(line));
    while(std::getline(file,line)) {
        if(line.size()>65536) {error="itemType-table-row-over-64KiB";return false;}
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(!line.empty())lines.push_back(std::move(line));
        if(lines.size()>16385) {error="itemType-table-over-16384-rows";return false;}
    }
    if(file.bad()) {error="itemType-table-read-failed";return false;}
    headings=SplitTsv(lines[0]);
    return true;
}
inline std::size_t Column(const std::vector<std::string_view>& header,
    std::string_view name) {
    for(std::size_t i=0;i<header.size();++i)
        if(header[i]==name)return i;
    return header.size();
}
inline bool ReadTypes(const fs::path& excel,Catalog& out,
    std::string& error) {
    std::vector<std::string> lines;
    std::vector<std::string_view> headings;
    if(!ReadRows(excel/L"itemtypes.txt",lines,headings,error))return false;
    // Some extracted D2R tables use Code/Equiv1/Equiv2 capitalization.
    const auto code=Column(headings,"Code"),name=Column(headings,"ItemType");
    const auto p1=Column(headings,"Equiv1"),p2=Column(headings,"Equiv2");
    if(code==headings.size() || name==headings.size() ||
       p1==headings.size() || p2==headings.size()) {
        error="itemType-requires-ItemType-Code-Equiv1-Equiv2-columns";
        return false;
    }
    for(std::size_t j=1;j<lines.size();++j) {
        const auto cells=SplitTsv(lines[j]);
        if(code>=cells.size() || cells[code].empty())continue;
        const auto id=TypeCode(cells[code]);
        if(id.empty()) {error="itemType-invalid-type-Code";return false;}
        const auto rawName=name<cells.size()?cells[name]:std::string_view{};
        if(rawName.size()>127 || rawName.find('\n')!=rawName.npos ||
           rawName.find('\r')!=rawName.npos) {
            error="itemType-invalid-ItemType-name";return false;
        }
        const auto parseParent=[&](std::size_t col,std::string& parent) {
            if(col>=cells.size() || cells[col].empty())return true;
            parent=TypeCode(cells[col]);return !parent.empty();
        };
        TypeRow row;
        if(!parseParent(p1,row.parent1) || !parseParent(p2,row.parent2)) {
            error="itemType-invalid-Equiv-code";return false;
        }
        if(!out.types.emplace(id,std::move(row)).second) {
            error="itemType-duplicate-Code:"+id;return false;
        }
        if(!rawName.empty())out.names[std::string(rawName)].push_back(id);
        ++out.typeRows;
    }
    if(!out.typeRows) {error="itemType-no-types";return false;}
    return true;
}
inline bool ReadItems(const fs::path& excel,const wchar_t* filename,
    Catalog& out,std::size_t& count,std::string& error,
    std::unordered_map<std::uint32_t,std::vector<std::string>>& direct) {
    std::vector<std::string> lines;
    std::vector<std::string_view> headings;
    if(!ReadRows(excel/filename,lines,headings,error))return false;
    const auto code=Column(headings,"code"),type=Column(headings,"type"),
        type2=Column(headings,"type2");
    if(code==headings.size() || type==headings.size()) {
        error="itemType-item-table-requires-code-and-type-columns:"+
            (excel/filename).filename().string();return false;
    }
    for(std::size_t j=1;j<lines.size();++j) {
        const auto cells=SplitTsv(lines[j]);
        if(code>=cells.size() || type>=cells.size() ||
           cells[code].empty() || cells[type].empty())continue;
        const auto packed=BaseNameTable::PackCode(cells[code]);
        const auto primary=TypeCode(cells[type]);
        if(!packed || primary.empty() || !out.types.contains(primary)) {
            error="itemType-invalid-item-code-or-unknown-primary-type";
            return false;
        }
        direct[packed].push_back(primary);
        if(type2<cells.size() && !cells[type2].empty()) {
            const auto secondary=TypeCode(cells[type2]);
            if(secondary.empty() || !out.types.contains(secondary)) {
                error="itemType-invalid-or-unknown-type2";return false;
            }
            direct[packed].push_back(secondary);
        }
        ++count;
    }
    return true;
}
// Cycle/unknown-parent checks are performed once at reload, not per label.
inline bool Ancestors(const Catalog& out,const std::string& id,
    std::unordered_map<std::string,std::vector<std::string>>& memo,
    std::unordered_set<std::string>& visiting,std::string& error) {
    if(memo.contains(id))return true;
    const auto row=out.types.find(id);
    if(row==out.types.end()) {error="itemType-unknown-Equiv:"+id;return false;}
    if(!visiting.insert(id).second) {
        error="itemType-cyclic-Equiv:"+id;return false;
    }
    std::vector<std::string> result{id};
    for(const auto* parent:{&row->second.parent1,&row->second.parent2}) {
        if(parent->empty())continue;
        if(!Ancestors(out,*parent,memo,visiting,error))return false;
        const auto& related=memo.at(*parent);
        result.insert(result.end(),related.begin(),related.end());
    }
    std::sort(result.begin(),result.end());
    result.erase(std::unique(result.begin(),result.end()),result.end());
    visiting.erase(id);
    memo.emplace(id,std::move(result));return true;
}
inline bool Load(const fs::path& excel,Catalog& out,std::string& error) {
    Catalog staged{};
    if(!ReadTypes(excel,staged,error))return false;
    std::unordered_map<std::uint32_t,std::vector<std::string>> direct;
    if(!ReadItems(excel,L"weapons.txt",staged,staged.weaponRows,error,direct) ||
       !ReadItems(excel,L"armor.txt",staged,staged.armorRows,error,direct) ||
       !ReadItems(excel,L"misc.txt",staged,staged.miscRows,error,direct))return false;
    std::unordered_map<std::string,std::vector<std::string>> memo;
    std::unordered_set<std::string> visiting;
    for(const auto& [id,row]:staged.types)
        if(!Ancestors(staged,id,memo,visiting,error))return false;
    for(const auto& [itemCode,types]:direct) {
        for(const auto& id:types) {
            for(const auto& ancestor:memo.at(id))
                staged.itemCodesByType[ancestor].push_back(itemCode);
        }
    }
    for(auto& [type,codes]:staged.itemCodesByType) {
        std::sort(codes.begin(),codes.end());
        codes.erase(std::unique(codes.begin(),codes.end()),codes.end());
    }
    // Literal ItemType names are aliases for the SAME resolved type sets.
    // Duplicate display names OR their codes; selectors are case-sensitive.
    for(const auto& [name,types]:staged.names) {
        if(staged.itemCodesByType.contains(name) && staged.types.contains(name))
            continue; // type code wins on exact code/name collision
        auto& codes=staged.itemCodesByType[name];
        for(const auto& type:types) {
            const auto it=staged.itemCodesByType.find(type);
            if(it!=staged.itemCodesByType.end())
                codes.insert(codes.end(),it->second.begin(),it->second.end());
        }
        std::sort(codes.begin(),codes.end());
        codes.erase(std::unique(codes.begin(),codes.end()),codes.end());
    }
    out=std::move(staged);return true;
}
} // namespace SoE::LootFilter::ItemTypeTable
