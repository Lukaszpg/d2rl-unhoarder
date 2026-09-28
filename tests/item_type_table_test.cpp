#include "../src/item_type_table.hpp"
#include "../src/filter_rule_engine.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
using namespace SoE::LootFilter;
namespace fs=std::filesystem;
static void Write(const fs::path& p,const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p,std::ios::binary);out<<s;assert(out.good());
}
static std::uint32_t Code(std::string_view t) {
    return BaseNameTable::PackCode(t);
}
int main() {
    const auto root=fs::temp_directory_path()/(
        "item-type-table-test-"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto excel=root/"test.mpq"/"data"/"global"/"excel";
    const auto types=excel/"itemtypes.txt";
    const std::string good=
        "\xef\xbb\xbf" "ItemType\tCode\tEquiv1\tEquiv2\tignored\r\n"
        "Weapon\tweap\t\t\tx\r\n"
        "Melee Weapon\tmele\tweap\t\tx\r\n"
        "Sword\tswor\tmele\t\tx\r\n"
        "One-Handed Sword\t1hs\tswor\t\tx\r\n"
        "Axe\taxe\tmele\t\tx\r\n"
        "Armor\tarmo\t\t\tx\r\n"
        "Body Armor\ttors\tarmo\t\tx\r\n"
        "Shield\tshld\tarmo\t\tx\r\n"
        "Miscellaneous\tmisc\t\t\tx\r\n"
        "Currency\tcurn\tmisc\t\tx\r\n"
        "Hybrid\thybr\tweap\tarmo\tx\r\n"
        "Oddity\todd\t\t\tx\r\n";
    Write(types,good);
    Write(excel/"weapons.txt",
        "name\ttype2\tcode\ttype\r\n"
        "Phase Blade\t\t7cr\t1hs\r\n"
        "Berserker Axe\t\t7wa\taxe\r\n"
        "Hybrid Weapon\tshld\thyb\thybr\r\n");
    Write(excel/"armor.txt",
        "code\ttype\ttype2\tname\n"
        "uar\ttors\t\tSacred Armor\n"
        "uit\tshld\t\tMonarch\n");
    Write(excel/"misc.txt",
        "code\tname\ttype\ttype2\n"
        "divo\tDivine Orb\tcurn\t\n"
        "char\tHybrid Charm\tmisc\todd\n");
    ItemTypeTable::Catalog catalog{};std::string error;
    assert(ItemTypeTable::Load(excel,catalog,error));
    assert(catalog.typeRows==12 && catalog.weaponRows==3 &&
           catalog.armorRows==2 && catalog.miscRows==2);
    const auto has=[&](std::string_view type,std::string_view code) {
        const auto* codes=catalog.Find(type);
        assert(codes);
        return std::find(codes->begin(),codes->end(),Code(code))!=codes->end();
    };
    assert(has("1hs","7cr") && has("swor","7cr"));
    assert(has("Sword","7cr") && has("Melee Weapon","7wa"));
    assert(has("weap","7cr") && has("weap","hyb"));
    assert(!has("swor","7wa"));
    assert(has("tors","uar") && has("Body Armor","uar"));
    assert(has("armo","uit") && has("armo","hyb"));
    assert(has("shld","hyb") && has("shld","uit"));
    assert(has("Currency","divo") && has("misc","divo"));
    assert(has("odd","char")); // optional type2
    assert(!catalog.Find("sword") && !catalog.Find("spurious"));
    RuleEngine::Conditions c{};
    c.typeCodes=*catalog.Find("swor");
    RuleEngine::Item item{};item.code=Code("7cr");
    assert(c.Matches(item));
    item.classIdKnown=true;item.classId=999;assert(c.Matches(item));
    item.code=Code("7wa");assert(!c.Matches(item));
    c.codes={Code("7cr")};assert(!c.Matches(item));
    item.code=Code("7cr");assert(c.Matches(item));
    // JSON resolver is expected to reject absent or ambiguous catalogs,
    // and never turn a no-item selector into an empty match-all condition.
    fs::remove(excel/"misc.txt");
    assert(!ItemTypeTable::Load(excel,catalog,error));
    Write(excel/"misc.txt","code\ttype\n" "divo\tcurn\n");
    Write(types,good+"Sword Loop\tloop\tloop\t\t\n");
    assert(!ItemTypeTable::Load(excel,catalog,error));
    Write(types,good+"Bad Parent\tbadp\tfake\t\t\n");
    assert(!ItemTypeTable::Load(excel,catalog,error));
    Write(types,good+"Dup\tswor\t\t\t\n");
    assert(!ItemTypeTable::Load(excel,catalog,error));
    fs::remove_all(root);
}
