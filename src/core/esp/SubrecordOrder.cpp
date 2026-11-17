#include "esp/RecordDefinitions.hpp"

#include <string_view>
#include <unordered_map>

namespace fo4mx {

namespace {

using RankTable = std::unordered_map<uint32_t, std::unordered_map<uint32_t, int>>;

const RankTable& rankTable()
{
    static const RankTable table = [] {
        RankTable t;
        auto order = [&t](std::string_view record, std::string_view subs) {
            auto& ranks = t[Signature::fromString(record).value];
            int rank = 0;
            for (size_t i = 0; i + 4 <= subs.size(); i += 5)
                ranks.emplace(Signature::fromString(subs.substr(i, 4)).value, rank++);
        };
#include "esp/SubrecordOrder.inc"
        return t;
    }();
    return table;
}

} // namespace

int subrecordRank(Signature record, Signature sub)
{
    const auto& table = rankTable();
    auto r = table.find(record.value);
    if (r == table.end())
        return -1;
    auto s = r->second.find(sub.value);
    return s == r->second.end() ? -1 : s->second;
}

} // namespace fo4mx
