#include "esp/RecordCatalogue.hpp"

#include "util/Binary.hpp"

#include <algorithm>
#include <charconv>

namespace fo4mx {

namespace embedded {
extern const char* const kRecordCataloguePieces[];
extern const std::size_t kRecordCataloguePieceCount;
} // namespace embedded

const char* placementName(RecordPlacement p)
{
    switch (p) {
    case RecordPlacement::Header: return "header";
    case RecordPlacement::Top: return "top";
    case RecordPlacement::Cell: return "cell";
    case RecordPlacement::CellChild: return "cell child";
    case RecordPlacement::TopicChild: return "topic child";
    }
    return "";
}

const char* strategyName(MergeStrategy s)
{
    switch (s) {
    case MergeStrategy::Specialised: return "Specialised";
    case MergeStrategy::Winner: return "Winner";
    case MergeStrategy::Candidate: return "Candidate";
    case MergeStrategy::Preserve: return "Preserve";
    }
    return "";
}

namespace {

// One CSV line: comma separated, fields optionally in double quotes ("" for a quote).
std::vector<std::string> splitCsv(std::string_view line)
{
    std::vector<std::string> out(1);
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                out.back() += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                out.back() += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            out.emplace_back();
        } else {
            out.back() += c;
        }
    }
    return out;
}

int parseOrder(const std::string& s, size_t line)
{
    if (s.empty())
        return -1;
    int v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size() || v < 0)
        throw FormatError("record catalogue line " + std::to_string(line) + ": bad number '" + s + "'");
    return v;
}

} // namespace

RecordCatalogue RecordCatalogue::parse(std::string_view csv)
{
    RecordCatalogue cat;
    bool header = true;
    size_t lineNo = 0;
    std::vector<std::pair<int, Signature>> grouped;
    while (!csv.empty()) {
        size_t nl = csv.find('\n');
        std::string_view line = csv.substr(0, nl);
        csv = nl == std::string_view::npos ? std::string_view() : csv.substr(nl + 1);
        ++lineNo;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.starts_with("\xEF\xBB\xBF"))
            line.remove_prefix(3);
        if (line.empty() || line.front() == '#')
            continue;
        if (header) { // column names
            header = false;
            continue;
        }
        auto f = splitCsv(line);
        auto fail = [&](const std::string& why) {
            throw FormatError("record catalogue line " + std::to_string(lineNo) + ": " + why);
        };
        if (f.size() < 5)
            fail("expected at least 5 columns");
        f.resize(8);
        if (f[0].size() != 4)
            fail("bad signature '" + f[0] + "'");
        CatalogueEntry e;
        e.sig = Signature::fromString(f[0]);
        e.name = f[1];
        e.category = f[2];
        if (f[3] == "header")
            e.placement = RecordPlacement::Header;
        else if (f[3] == "top")
            e.placement = RecordPlacement::Top;
        else if (f[3] == "cell")
            e.placement = RecordPlacement::Cell;
        else if (f[3] == "cell child")
            e.placement = RecordPlacement::CellChild;
        else if (f[3] == "topic child")
            e.placement = RecordPlacement::TopicChild;
        else
            fail("unknown placement '" + f[3] + "'");
        if (f[4] == "Specialised")
            e.strategy = MergeStrategy::Specialised;
        else if (f[4] == "Winner")
            e.strategy = MergeStrategy::Winner;
        else if (f[4] == "Candidate")
            e.strategy = MergeStrategy::Candidate;
        else if (f[4] == "Preserve")
            e.strategy = MergeStrategy::Preserve;
        else
            fail("unknown strategy '" + f[4] + "'");
        e.groupOrder = parseOrder(f[5], lineNo);
        e.allocationOrder = parseOrder(f[6], lineNo);
        e.notes = f[7];
        if (cat.index_.count(e.sig))
            fail("duplicate signature " + f[0]);
        if (e.groupOrder >= 0)
            grouped.emplace_back(e.groupOrder, e.sig);
        cat.allocationGroups_ = std::max(cat.allocationGroups_, e.allocationOrder + 2);
        cat.index_[e.sig] = cat.entries_.size();
        cat.entries_.push_back(std::move(e));
    }
    std::stable_sort(grouped.begin(), grouped.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [_, s] : grouped)
        cat.groupOrder_.push_back(s);
    return cat;
}

const RecordCatalogue& RecordCatalogue::get()
{
    static const RecordCatalogue catalogue = [] {
        std::string text;
        for (size_t i = 0; i < embedded::kRecordCataloguePieceCount; ++i)
            text += embedded::kRecordCataloguePieces[i];
        return parse(text);
    }();
    return catalogue;
}

const CatalogueEntry* RecordCatalogue::find(Signature s) const
{
    auto it = index_.find(s);
    return it == index_.end() ? nullptr : &entries_[it->second];
}

} // namespace fo4mx
