#pragma once
// Ground-item quantity prefix: "12x Item Name" when quantity > 1.
// Used by the qualified visible ground-label writer.
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <array>

namespace SoE::LootFilter::GroundQuantity {
inline bool HasCountSuffix(std::string_view text) noexcept {
    if (text.size()<4 || text.back()!=')') return false;
    const auto open=text.rfind(" (");
    if (open==text.npos || open+3>=text.size()) return false;
    for (std::size_t i=open+2;i+1<text.size();++i)
        if (text[i]<'0'||text[i]>'9') return false;
    return true;
}
inline bool HasCountPrefix(std::string_view text) noexcept {
    std::size_t n{};
    while (n<text.size() && text[n]>='0' && text[n]<='9') ++n;
    return n>0 && n+2<text.size() && text[n]=='x' &&
        text[n+1]==' ';
}
inline bool Append(std::string_view name,std::uint32_t quantity,
                   char* output,std::size_t capacity,std::size_t& bytes) noexcept {
    bytes=0;
    if (!output || quantity<=1 || quantity>65535 || name.empty() ||
        HasCountPrefix(name) || name.find('\0')!=name.npos ||
        name.find('\r')!=name.npos || name.find('\n')!=name.npos)
        return false;
    // If upstream has already attached our previous " (N)" presentation,
    // convert it instead of leaving an old suffix or doubling the count.
    if (HasCountSuffix(name)) {
        const auto open=name.rfind(" (");
        std::uint32_t previous{};
        const auto digits=name.substr(open+2,name.size()-open-3);
        const auto [end,ec]=std::from_chars(digits.data(),
            digits.data()+digits.size(),previous);
        if (ec!=std::errc{} || end!=digits.data()+digits.size() ||
            previous!=quantity) return false;
        name.remove_suffix(name.size()-open);
        if (name.empty()) return false;
    }
    char number[6]{};
    const auto [end,error]=std::to_chars(number,number+sizeof(number),quantity);
    if (error!=std::errc{}) return false;
    const auto digits=static_cast<std::size_t>(end-number);
    if (name.size()>capacity || capacity-name.size()<digits+3U) return false;
    std::memcpy(output,number,digits);
    bytes=digits;
    output[bytes++]='x';output[bytes++]=' ';
    std::memcpy(output+bytes,name.data(),name.size());bytes+=name.size();
    output[bytes]='\0';
    return true;
}
// Compare the exact native visible label against the configured item name.
// A formatter-qualified quantity is required: a matching "12x " prefix is
// permitted only when the same borrowed native item reported quantity 12.
// Never strip arbitrary count-like text and then recolor by name alone.
inline bool MatchesRuleName(std::string_view displayed,
                            std::string_view configured,
                            std::uint32_t quantity) noexcept {
    if (displayed.empty() || configured.empty()) return false;
    if (displayed==configured) return true; // single / no stat bridge
    std::array<char,256> expected{};
    std::size_t size{};
    return Append(configured,quantity,expected.data(),expected.size(),size) &&
        displayed==std::string_view(expected.data(),size);
}

} // namespace SoE::LootFilter::GroundQuantity
