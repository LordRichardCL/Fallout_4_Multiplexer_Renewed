#include "engine/MergeEngine.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>

namespace fo4mx {

const char* cellStatusName(CellStatus s)
{
    switch (s) {
    case CellStatus::Base: return "Base";
    case CellStatus::Same: return "Same";
    case CellStatus::Winning: return "Winning";
    case CellStatus::Merged: return "Merged";
    case CellStatus::Replaced: return "Replaced";
    case CellStatus::Absent: return "Absent";
    }
    return "";
}

namespace {

// Ordered union of field keys: the winner's order, other keys inserted after
// their predecessor in the plugin that introduced them.
std::vector<std::string> unionKeys(const std::vector<FieldSet>& chain)
{
    std::vector<std::string> order;
    for (const auto& f : chain.back().fields)
        order.push_back(f.key);
    for (size_t c = chain.size(); c-- > 0;) {
        const auto& fs = chain[c].fields;
        for (size_t i = 0; i < fs.size(); ++i) {
            if (std::find(order.begin(), order.end(), fs[i].key) != order.end())
                continue;
            auto pos = order.begin();
            for (size_t p = i; p-- > 0;) {
                auto it = std::find(order.begin(), order.end(), fs[p].key);
                if (it != order.end()) {
                    pos = it + 1;
                    break;
                }
            }
            order.insert(pos, fs[i].key);
        }
    }
    return order;
}

const Field* templateField(const std::vector<FieldSet>& chain, const std::string& key)
{
    for (size_t c = chain.size(); c-- > 0;)
        if (auto f = chain[c].find(key))
            return f;
    return nullptr;
}

// Merges list fields: start with the base items and replay every changer's
// additions and removals in load order.
Field mergeList(const std::vector<const Field*>& values, const std::vector<size_t>& changers, const Field& shape,
                bool& hard)
{
    Field result = shape;
    result.items.clear();
    const Field* base = values[0];
    if (base)
        result.items = base->items;
    result.hadCount = false;
    for (const auto* v : values)
        if (v && v->hadCount)
            result.hadCount = true;

    if (shape.merge == MergeMode::Keyed) {
        // key -> (value, set by)
        std::map<FormKey, std::pair<ListItem, size_t>> keyed;
        std::vector<FormKey> order;
        if (base)
            for (const auto& it : base->items) {
                auto k = itemKey(it);
                if (keyed.emplace(k, std::make_pair(it, size_t(0))).second)
                    order.push_back(k);
            }
        std::map<FormKey, ListItem> baseValues;
        if (base)
            for (const auto& it : base->items)
                baseValues.emplace(itemKey(it), it);

        for (size_t c : changers) {
            const Field* v = values[c];
            std::set<FormKey> present;
            if (v) {
                for (const auto& it : v->items) {
                    auto k = itemKey(it);
                    present.insert(k);
                    auto bit = baseValues.find(k);
                    bool changed = bit == baseValues.end() || !itemEquivalent(bit->second, it);
                    if (!changed)
                        continue;
                    auto cur = keyed.find(k);
                    if (cur == keyed.end()) {
                        keyed.emplace(k, std::make_pair(it, c));
                        order.push_back(k);
                    } else {
                        if (cur->second.second != 0 && !itemEquivalent(cur->second.first, it))
                            hard = true;
                        cur->second = {it, c};
                    }
                }
            }
            for (const auto& [k, _] : baseValues)
                if (!present.count(k))
                    keyed.erase(k);
        }
        for (const auto& k : order) {
            auto it = keyed.find(k);
            if (it != keyed.end())
                result.items.push_back(it->second.first);
        }
        return result;
    }

    // Set / Multiset.
    for (size_t c : changers) {
        const Field* v = values[c];
        std::vector<ListItem> baseRemaining = base ? base->items : std::vector<ListItem>{};
        std::vector<ListItem> additions;
        if (v) {
            for (const auto& it : v->items) {
                auto match = std::find_if(baseRemaining.begin(), baseRemaining.end(),
                                          [&](const ListItem& b) { return itemEquivalent(b, it); });
                if (match != baseRemaining.end())
                    baseRemaining.erase(match);
                else
                    additions.push_back(it);
            }
        }
        // baseRemaining now holds the base items this changer removed.
        for (const auto& removed : baseRemaining) {
            auto match = std::find_if(result.items.begin(), result.items.end(),
                                      [&](const ListItem& r) { return itemEquivalent(r, removed); });
            if (match != result.items.end())
                result.items.erase(match);
        }
        for (const auto& add : additions) {
            if (shape.merge == MergeMode::Set) {
                bool exists = std::any_of(result.items.begin(), result.items.end(),
                                          [&](const ListItem& r) { return itemEquivalent(r, add); });
                if (exists)
                    continue;
            }
            result.items.push_back(add);
        }
    }
    return result;
}

} // namespace

ChainAnalysis MergeEngine::analyse(const std::vector<FieldSet>& chain)
{
    MergeResult r = merge(chain, MergeOptions{});
    return r.analysis;
}

MergeResult MergeEngine::merge(const std::vector<FieldSet>& chain, const MergeOptions& options)
{
    MergeResult result;
    const size_t n = chain.size();
    if (n == 0)
        return result;
    const size_t winner = n - 1;

    std::set<size_t> changerSet;
    std::vector<bool> anyChange(n, false);

    FieldSet out;
    out.key = chain[winner].key;
    out.sig = chain[winner].sig;
    out.plugin = chain[winner].plugin;
    out.formVersion = chain[winner].formVersion;

    for (const auto& key : unionKeys(chain)) {
        std::vector<const Field*> values(n);
        for (size_t c = 0; c < n; ++c)
            values[c] = chain[c].find(key);

        FieldAnalysis fa;
        const Field* shape = templateField(chain, key);
        fa.key = key;
        fa.label = shape->label;
        fa.listField = shape->isList();
        for (size_t c = 1; c < n; ++c)
            if (!fieldEquivalent(values[0], values[c]))
                fa.changers.push_back(c);

        // Hard conflict: two changers with different values for a scalar field.
        if (!fa.listField)
            for (size_t a = 0; a < fa.changers.size() && !fa.hard; ++a)
                for (size_t b = a + 1; b < fa.changers.size(); ++b)
                    if (!fieldEquivalent(values[fa.changers[a]], values[fa.changers[b]])) {
                        fa.hard = true;
                        break;
                    }

        Field merged;
        bool present = true;
        if (options.forcedWinner >= 0) {
            const Field* v = values[static_cast<size_t>(options.forcedWinner)];
            present = v != nullptr;
            if (v)
                merged = *v;
            fa.source = static_cast<size_t>(options.forcedWinner);
        } else if (!options.mergeCompatible || fa.changers.empty()) {
            const Field* v = options.mergeCompatible ? values[0] : values[winner];
            present = v != nullptr;
            if (v)
                merged = *v;
            fa.source = options.mergeCompatible ? 0 : winner;
        } else if (fa.listField) {
            bool listHard = false;
            merged = mergeList(values, fa.changers, *shape, listHard);
            fa.hard = fa.hard || listHard;
            fa.source = fa.changers.back();
            present = !merged.items.empty() || values[winner] != nullptr;
        } else {
            size_t last = fa.changers.back();
            present = values[last] != nullptr;
            if (present)
                merged = *values[last];
            fa.source = last;
        }

        const Field* resultPtr = present ? &merged : nullptr;
        fa.fromNonWinner = !fieldEquivalent(resultPtr, values[winner]);
        fa.cells.resize(n);
        for (size_t c = 0; c < n; ++c) {
            if (!values[c])
                fa.cells[c] = fieldEquivalent(nullptr, resultPtr) ? CellStatus::Same : CellStatus::Absent;
            else if (!fieldEquivalent(values[c], resultPtr))
                fa.cells[c] = CellStatus::Replaced;
            else if (c == fa.source && !fa.changers.empty())
                fa.cells[c] = (c == winner) ? CellStatus::Winning : CellStatus::Merged;
            else if (c == winner)
                fa.cells[c] = CellStatus::Winning;
            else
                fa.cells[c] = c == 0 ? CellStatus::Base : CellStatus::Same;
        }

        for (size_t c : fa.changers) {
            changerSet.insert(c);
            anyChange[c] = true;
        }
        if (!fa.changers.empty()) {
            result.analysis.hard |= fa.hard;
            if (fa.fromNonWinner)
                result.merged = true;
            result.analysis.fields.push_back(fa);
        }
        if (present)
            out.fields.push_back(std::move(merged));
    }

    result.analysis.changers.assign(changerSet.begin(), changerSet.end());
    for (size_t c = 1; c < n; ++c)
        if (!anyChange[c])
            result.analysis.itms.push_back(c);
    // With merging on, a result that differs from the last plugin means last-plugin-wins loses edits.
    result.analysis.winnerLosesChanges = options.mergeCompatible && options.forcedWinner < 0 && result.merged;
    result.contributors = result.analysis.changers;
    if (options.forcedWinner >= 0)
        result.merged = static_cast<size_t>(options.forcedWinner) != winner;
    result.fields = std::move(out);
    return result;
}

} // namespace fo4mx
