#include "diagnostics/PapyrusLog.hpp"

#include "engine/CompilationEngine.hpp"
#include "engine/Session.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>

namespace fo4mx::diagnostics {

std::pair<std::string, uint32_t> RuntimeLoadOrder::resolve(uint32_t id) const
{
    const uint32_t top = id >> 24;
    if (top == 0xFF)
        return {"(created in game)", id & 0xFFFFFF};
    if (top == 0xFE) {
        const uint32_t slot = (id >> 12) & 0xFFF;
        return {slot < light.size() ? light[slot] : std::string(), id & 0xFFF};
    }
    return {top < full.size() ? full[top] : std::string(), id & 0xFFFFFF};
}

namespace {

std::string upper(std::string s)
{
    for (auto& ch : s)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

std::string hex6(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06X", v);
    return buf;
}

// "error: Cannot call X() on a None object" without the time stamp; the message part after "error: ".
std::string messageOf(const std::string& line, size_t at)
{
    std::string m = line.substr(at);
    while (!m.empty() && (m.back() == '\r' || m.back() == ' '))
        m.pop_back();
    return m;
}

} // namespace

namespace {

bool scriptChar(char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == ':';
}

// The script names a line gives: "].Script.Function(" in a stack, class "X" in "Cannot open store", "X.psc".
std::vector<std::pair<std::string, bool>> scriptsIn(const std::string& line) // (name, missing)
{
    std::vector<std::pair<std::string, bool>> out;
    for (size_t p = line.find("]."); p != std::string::npos; p = line.find("].", p + 2)) {
        size_t e = p + 2;
        while (e < line.size() && scriptChar(line[e]))
            ++e;
        if (e < line.size() && line[e] == '.' && e > p + 2)
            out.push_back({line.substr(p + 2, e - p - 2), false});
    }
    const std::string lower = toLower(line);
    if (auto at = lower.find("cannot open store for class \""); at != std::string::npos) {
        const size_t b = at + 29;
        const size_t e = line.find('"', b);
        if (e != std::string::npos && e > b)
            out.push_back({line.substr(b, e - b), true});
    }
    for (size_t p = lower.find(".psc\""); p != std::string::npos; p = lower.find(".psc\"", p + 5)) {
        size_t b = p;
        while (b > 0 && line[b - 1] != '"' && line[b - 1] != '\\' && line[b - 1] != '/')
            --b;
        if (p > b)
            out.push_back({line.substr(b, p - b), false});
    }
    return out;
}

} // namespace

ScriptOwner scriptOwnerOf(const Session& session)
{
    return [&session](const std::string& path) -> std::string {
        if (const auto* e = session.assets.find(path))
            return session.assets.modName(e->winner());
        if (const auto* f = session.vfs.find(path))
            return session.vfs.winnerName(*f);
        return {};
    };
}

PapyrusReport translatePapyrusLog(std::string_view text, const RuntimeLoadOrder& lo,
                                  const std::vector<FormMapping>& formMap, const std::set<std::string>& redirectedIn,
                                  const ScriptOwner& scriptOwner)
{
    PapyrusReport r;
    std::map<std::string, size_t> scriptIndex;
    std::set<std::string> outputs, redirected;
    std::map<std::pair<std::string, uint32_t>, const FormMapping*> byOutput;
    for (const auto& m : formMap) {
        outputs.insert(toLower(m.outputPlugin));
        byOutput[{toLower(m.outputPlugin), m.objectId & 0xFFFFFF}] = &m;
    }
    for (const auto& s : redirectedIn)
        redirected.insert(toLower(s));
    std::map<std::string, size_t> refIndex;
    std::map<std::string, size_t> messageIndex;
    bool inStack = false; // the "stack:" lines under an error belong to it
    for (const auto& raw : split(std::string(text), '\n', false)) {
        const std::string line = raw;
        if (line.empty())
            continue;
        ++r.lines;
        const std::string lower = toLower(line);
        bool isError = false;
        if (auto at = lower.find("] error: "); at != std::string::npos) {
            ++r.errors;
            isError = true;
            inStack = true;
            const std::string msg = messageOf(line, at + 9);
            // Count by message without its FormIDs and numbers, so one error on many objects is one row.
            std::string key;
            for (char ch : msg)
                key += std::isdigit(static_cast<unsigned char>(ch)) ? '#' : ch;
            auto [it, fresh] = messageIndex.try_emplace(key, r.topErrors.size());
            if (fresh)
                r.topErrors.push_back({msg, 0});
            ++r.topErrors[it->second].count;
        } else if (lower.find("] warning: ") != std::string::npos) {
            ++r.warnings;
            inStack = false;
        } else if (lower.find("papyrus log opened") != std::string::npos ||
                   lower.find("virtual machine") != std::string::npos) {
            r.recognised = true;
            inStack = false;
        } else if (!line.empty() && line[0] == '[') {
            inStack = false;
        }
        if (isError || r.errors || r.warnings)
            r.recognised = true;
        {
            std::set<std::string> seenHere;
            for (const auto& [name, missing] : scriptsIn(line)) {
                const std::string key = toLower(name);
                if (!seenHere.insert(key).second)
                    continue;
                auto [it, fresh] = scriptIndex.try_emplace(key, r.scripts.size());
                if (fresh) {
                    PapyrusScript sc;
                    sc.name = name;
                    if (scriptOwner) {
                        std::string path = "scripts\\" + key + ".pex";
                        std::replace(path.begin(), path.end(), ':', '\\');
                        sc.mod = scriptOwner(path);
                    }
                    r.scripts.push_back(std::move(sc));
                }
                auto& sc = r.scripts[it->second];
                ++sc.mentions;
                if (isError || inStack)
                    ++sc.inErrors;
                sc.missing |= missing;
            }
        }
        // "(0A001234)": 8 hex digits in parentheses.
        for (size_t p = line.find('('); p != std::string::npos; p = line.find('(', p + 1)) {
            if (p + 10 > line.size() || line[p + 9] != ')')
                continue;
            const std::string hex = line.substr(p + 1, 8);
            if (!std::all_of(hex.begin(), hex.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)); }))
                continue;
            const auto id = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
            const std::string key = upper(hex);
            auto [it, fresh] = refIndex.try_emplace(key, r.refs.size());
            if (fresh) {
                PapyrusFormRef ref;
                ref.formId = key;
                // "[Name (" or "<Name (", or "[Type < (" (no name) before it.
                auto trimmed = [](std::string t) {
                    while (!t.empty() && t.back() == ' ')
                        t.pop_back();
                    while (!t.empty() && t.front() == ' ')
                        t.erase(t.begin());
                    return t;
                };
                const size_t lt = line.rfind('<', p);
                const size_t lb = line.rfind('[', p);
                const size_t open = lt == std::string::npos ? lb : lb == std::string::npos ? lt : std::max(lt, lb);
                if (open != std::string::npos) {
                    ref.name = trimmed(line.substr(open + 1, p - open - 1));
                    if (ref.name.empty() && open == lt && lb != std::string::npos && lb < lt)
                        ref.name = trimmed(line.substr(lb + 1, lt - lb - 1));
                }
                ref.firstLine = line;
                const auto [plugin, object] = lo.resolve(id);
                ref.plugin = plugin;
                ref.objectId = object;
                const std::string pk = toLower(plugin);
                ref.redirected = redirected.count(pk) != 0;
                if (outputs.count(pk)) {
                    ref.inOutput = true;
                    if (auto m = byOutput.find({pk, object}); m != byOutput.end()) {
                        ref.source = m->second->sourcePlugin;
                        ref.sourceObjectId = m->second->sourceObjectId;
                        ref.editorId = m->second->editorId;
                        ref.signature = m->second->sig.str();
                    }
                }
                r.refs.push_back(std::move(ref));
            }
            auto& ref = r.refs[it->second];
            ++ref.mentions;
            if (isError || inStack)
                ++ref.inErrors;
        }
    }
    for (const auto& ref : r.refs)
        if (ref.inOutput)
            ++(ref.source.empty() ? r.untranslated : r.translated);
    std::stable_sort(r.scripts.begin(), r.scripts.end(), [](const PapyrusScript& a, const PapyrusScript& b) {
        if (a.inErrors != b.inErrors)
            return a.inErrors > b.inErrors;
        return a.mentions > b.mentions;
    });
    std::stable_sort(r.topErrors.begin(), r.topErrors.end(),
                     [](const PapyrusMessage& a, const PapyrusMessage& b) { return a.count > b.count; });
    std::stable_sort(r.refs.begin(), r.refs.end(), [](const PapyrusFormRef& a, const PapyrusFormRef& b) {
        const bool ta = !a.source.empty(), tb = !b.source.empty();
        if (ta != tb)
            return ta;
        if (a.inErrors != b.inErrors)
            return a.inErrors > b.inErrors;
        return a.mentions > b.mentions;
    });
    return r;
}

std::string formatPapyrusReport(const PapyrusReport& r, size_t limit)
{
    if (!r.recognised)
        return "This does not look like a Papyrus log (no Papyrus lines, errors or warnings).\n";
    std::string o = std::to_string(r.lines) + " lines, " + std::to_string(r.errors) + " errors, " +
                    std::to_string(r.warnings) + " warnings; " + std::to_string(r.refs.size()) +
                    " objects named by FormID, " + std::to_string(r.translated) +
                    " of them compiled records translated to their original mod" +
                    (r.untranslated ? " (" + std::to_string(r.untranslated) +
                                          " in an output but not in this build's map: another build?)"
                                    : std::string()) +
                    ".\nFormIDs are read with the current load order: if it changed since the log was written, "
                    "rebuild the reading after switching back to that load order.\n";
    if (!r.topErrors.empty()) {
        o += "\nMost frequent errors:\n";
        for (size_t i = 0; i < r.topErrors.size() && i < 20; ++i)
            o += "  " + std::to_string(r.topErrors[i].count) + "x  " + r.topErrors[i].text + "\n";
    }
    {
        std::vector<const PapyrusScript*> list;
        for (const auto& sc : r.scripts)
            if (sc.inErrors || sc.missing)
                list.push_back(&sc);
        if (!list.empty()) {
            o += "\nScripts in errors, and the mod that provides each:\n";
            for (size_t i = 0; i < list.size() && i < limit; ++i) {
                const auto& sc = *list[i];
                o += "  " + sc.name + "  " +
                     (sc.mod.empty() ? std::string("(no installed mod provides it)") : sc.mod) + "  (" +
                     std::to_string(sc.inErrors) + " in errors)" +
                     (sc.missing ? std::string("  the game could not load it") : std::string()) + "\n";
            }
            if (list.size() > limit)
                o += "  ... and " + std::to_string(list.size() - limit) + " more\n";
        }
    }
    if (!r.refs.empty()) {
        o += "\nObjects (compiled records first, then those in errors):\n";
        for (size_t i = 0; i < r.refs.size() && i < limit; ++i) {
            const auto& ref = r.refs[i];
            o += "\n  " + ref.formId + "  " + (ref.plugin.empty() ? std::string("(not in the current load order)") : ref.plugin) +
                 (ref.plugin.empty() ? "" : " " + hex6(ref.objectId)) + (ref.name.empty() ? "" : "  " + ref.name) +
                 "  (" + std::to_string(ref.mentions) + " mention(s)" +
                 (ref.inErrors ? ", " + std::to_string(ref.inErrors) + " in errors" : std::string()) + ")\n";
            if (!ref.source.empty())
                o += "      compiled from " + ref.source + " " + hex6(ref.sourceObjectId) +
                     (ref.editorId.empty() ? "" : " (" + ref.editorId + ")") +
                     (ref.signature.empty() ? "" : " [" + ref.signature + "]") + ": look at that mod first\n";
            else if (ref.inOutput)
                o += "      in an output plugin, but not in this build's FormID map\n";
            if (ref.redirected)
                o += "      " + ref.plugin + " is a redirected copy (Master Redirection) of the original plugin\n";
        }
        if (r.refs.size() > limit)
            o += "\n  ... and " + std::to_string(r.refs.size() - limit) + " more\n";
    }
    return o;
}

RuntimeLoadOrder runtimeLoadOrder(const Session& session)
{
    RuntimeLoadOrder lo;
    for (auto pi : session.loadOrder) {
        const auto& p = session.plugins[pi];
        (p.isLight() ? lo.light : lo.full).push_back(p.name);
    }
    return lo;
}

bool readBuildMap(const std::filesystem::path& root, std::vector<FormMapping>& map, std::set<std::string>& redirected)
{
    std::error_code ec;
    for (const auto& dir : {root / "Data", root}) {
        if (root.empty() || !std::filesystem::exists(dir / "Multiplexer_FormMap.tsv", ec))
            continue;
        map = CompilationEngine::readFormMap(dir / "Multiplexer_FormMap.tsv");
        std::ifstream r(dir / "Multiplexer_Redirected.tsv");
        for (std::string line; std::getline(r, line);)
            if (!line.empty() && line[0] != '#')
                redirected.insert(line.substr(0, line.find('\t')));
        return true;
    }
    return false;
}

std::filesystem::path defaultPapyrusLog()
{
    return platform::fallout4DocumentsDir() / "Logs" / "Script" / "Papyrus.0.log";
}

} // namespace fo4mx::diagnostics
