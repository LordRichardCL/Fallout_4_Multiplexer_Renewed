#include "engine/BuildComparison.hpp"

#include "engine/EarlierBuilds.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace fo4mx {

namespace {

std::string hex6(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06X", v & 0xFFFFFF);
    return buf;
}

using SourceKey = std::pair<std::string, uint32_t>;

SourceKey keyOf(const FormMapping& m) { return {toLower(m.sourcePlugin), m.sourceObjectId}; }

std::vector<std::string> minus(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    std::set<std::string> in;
    for (const auto& s : b)
        in.insert(toLower(s));
    std::vector<std::string> out;
    for (const auto& s : a)
        if (!in.count(toLower(s)))
            out.push_back(s);
    return out;
}

std::string label(const FormMapping& m)
{
    return m.sourcePlugin + " " + hex6(m.sourceObjectId) + (m.editorId.empty() ? "" : " " + m.editorId) +
           (m.sig.str().empty() ? "" : " [" + m.sig.str() + "]");
}

} // namespace

BuildComparison compareBuilds(const BuildSide& before, const BuildSide& after)
{
    BuildComparison c;
    std::map<SourceKey, const FormMapping*> old;
    for (const auto& m : before.mappings)
        old[keyOf(m)] = &m;
    std::set<SourceKey> seen;
    for (const auto& m : after.mappings) {
        const auto k = keyOf(m);
        seen.insert(k);
        auto it = old.find(k);
        if (it == old.end()) {
            c.added.push_back(m);
            continue;
        }
        const auto& b = *it->second;
        if (!iequals(b.outputPlugin, m.outputPlugin))
            c.moved.push_back({b, m});
        else if ((b.objectId & 0xFFFFFF) != (m.objectId & 0xFFFFFF))
            c.renumbered.push_back({b, m});
        else
            ++c.kept;
    }
    for (const auto& m : before.mappings)
        if (!seen.count(keyOf(m)))
            c.removed.push_back(m);
    c.newlyCompiled = minus(after.compiled, before.compiled);
    c.returned = minus(before.compiled, after.compiled);
    c.outputsAdded = minus(after.outputs, before.outputs);
    c.outputsRemoved = minus(before.outputs, after.outputs);
    return c;
}

bool readBuildSide(const std::filesystem::path& dir, BuildSide& out)
{
    const auto map = dir / "Multiplexer_FormMap.tsv";
    std::error_code ec;
    if (dir.empty() || !std::filesystem::exists(map, ec))
        return false;
    out.mappings = CompilationEngine::readFormMap(map);
    if (auto r = earlier::readCompiledList(dir / earlier::kCompiledListFile)) {
        out.compiled = r->compiled;
        out.outputs = r->outputs;
    } else {
        std::set<std::string> outs;
        for (const auto& m : out.mappings)
            if (outs.insert(toLower(m.outputPlugin)).second)
                out.outputs.push_back(m.outputPlugin);
    }
    return true;
}

std::string formatBuildComparison(const BuildComparison& c, size_t limit)
{
    std::string o;
    o += "Compared with the previous build" + (c.previousSource.empty() ? std::string() : " (" + c.previousSource + ")") +
         ":\n";
    if (c.same()) {
        o += "  No change: every record keeps its output plugin and FormID, and the same plugins compile.\n";
        return o;
    }
    o += "  " + std::to_string(c.kept) + " record(s) keep their FormID\n";
    o += "  " + std::to_string(c.added.size()) + " new record(s)\n";
    o += "  " + std::to_string(c.moved.size()) + " moved to another output plugin (new FormID)\n";
    o += "  " + std::to_string(c.renumbered.size()) + " renumbered in the same output plugin (new FormID)\n";
    o += "  " + std::to_string(c.removed.size()) + " no longer in the build\n";
    if (c.formIdChanges())
        o += "  Saves made with the previous build lose the moved, renumbered and removed records.\n";
    auto list = [&](const std::string& title, const std::vector<std::string>& v) {
        if (v.empty())
            return;
        o += "\n" + title + " (" + std::to_string(v.size()) + "):\n";
        for (size_t i = 0; i < v.size() && i < limit; ++i)
            o += "  " + v[i] + "\n";
        if (v.size() > limit)
            o += "  ... and " + std::to_string(v.size() - limit) + " more\n";
    };
    list("Plugins compiled for the first time", c.newlyCompiled);
    list("Plugins back in the load order (no longer compiled)", c.returned);
    list("New output plugins", c.outputsAdded);
    list("Output plugins no longer produced", c.outputsRemoved);
    auto changes = [&](const std::string& title, const std::vector<BuildComparison::Change>& v) {
        std::vector<std::string> lines;
        for (const auto& ch : v)
            lines.push_back(label(ch.after) + ": " + ch.before.outputPlugin + " " + hex6(ch.before.objectId) + " -> " +
                            ch.after.outputPlugin + " " + hex6(ch.after.objectId));
        list(title, lines);
    };
    changes("Moved to another output plugin", c.moved);
    changes("Renumbered", c.renumbered);
    auto records = [&](const std::string& title, const std::vector<FormMapping>& v) {
        std::vector<std::string> lines;
        for (const auto& m : v)
            lines.push_back(label(m) + ": " + m.outputPlugin + " " + hex6(m.objectId));
        list(title, lines);
    };
    records("No longer in the build", c.removed);
    records("New records", c.added);
    return o;
}

} // namespace fo4mx
