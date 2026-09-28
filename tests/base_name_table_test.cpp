#include "../src/base_name_table.hpp"
#include "../src/filter_rule_engine.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
using namespace SoE::LootFilter;
namespace fs=std::filesystem;
static void Write(const fs::path& path,const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::binary);out<<text;assert(out.good());
}
int main() {
    const auto temp=fs::temp_directory_path()/
        ("loot-filter-base-name-test-"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto excel=temp/"mods"/"soe-resurrected"/
        "soe-resurrected.mpq"/"data"/"global"/"excel";
    const auto config=temp/"mods"/"soe-resurrected"/"d2rloader"/
        "plugins"/"loot-filter.json";
    Write(excel/"weapons.txt",
        "\xef\xbb\xbf" "name\tfoo\tcode\r\n"
        "Berserker Axe\tunused\t7wa\r\n"
        "Twin Base\tunused\tabc\r\n"
        "\tunused\t\r\n");
    Write(excel/"armor.txt",
        "code\tname\tfoo\r\n"
        "uar\tSacred Armor\tunused\r\n"
        "utp\tArchon Plate\tunused\r\n"
        "def\tTwin Base\tunused\r\n");
    assert(BaseNameTable::DiscoverExcel(config)==excel);
    BaseNameTable::Catalog names{};std::string error;
    assert(BaseNameTable::LoadPair(excel,names,error));
    assert(names.weaponRows==2 && names.armorRows==3);
    const auto* sacred=names.Find("Sacred Armor");
    assert(sacred && sacred->size()==1 && sacred->front()==BaseNameTable::PackCode("uar"));
    const auto* axe=names.Find("Berserker Axe");
    assert(axe && axe->front()==BaseNameTable::PackCode("7wa"));
    const auto* twins=names.Find("Twin Base");
    assert(twins && twins->size()==2);
    assert(!names.Find("sacred armor")); // literal exact-case data column
    assert(!names.Find("Divine Orb")); // intentionally no Misc.txt
    assert(BaseNameTable::PackCode("uar ")==BaseNameTable::PackCode("uar"));
    RuleEngine::Conditions c{};c.baseCodes=*sacred;
    RuleEngine::Item item{};item.code=BaseNameTable::PackCode("uar");
    assert(c.Matches(item));
    item.classIdKnown=true;item.classId=999;assert(c.Matches(item));
    item.code=BaseNameTable::PackCode("utp");assert(!c.Matches(item));
    // Missing, malformed, and wrong-column files may not produce a partial
    // catalog which could accidentally hide an unrelated item.
    fs::remove(excel/"armor.txt");
    assert(!BaseNameTable::LoadPair(excel,names,error));
    Write(excel/"armor.txt","name\twrong\nSacred Armor\tuar\n");
    assert(!BaseNameTable::LoadPair(excel,names,error));
    fs::remove_all(temp);
}
