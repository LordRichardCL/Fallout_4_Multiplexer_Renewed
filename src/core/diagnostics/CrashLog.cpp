#include "diagnostics/CrashLog.hpp"

#include "engine/CompilationEngine.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <cstdio>
#include <map>

namespace fo4mx::diagnostics {

namespace fs = std::filesystem;

namespace {

std::string trim(std::string_view s)
{
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r");
    return std::string(s.substr(b, e - b + 1));
}

std::string unquote(std::string s)
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

std::string hex8(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

std::string hex6(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06X", v);
    return buf;
}

} // namespace

CrashReport translateCrashLog(std::string_view text, const std::vector<FormMapping>& formMap,
                              const std::set<std::string>& outputsIn, const std::set<std::string>& redirectedIn)
{
    CrashReport r;
    std::set<std::string> outputs, redirected;
    for (const auto& o : outputsIn)
        outputs.insert(toLower(o));
    for (const auto& m : formMap)
        outputs.insert(toLower(m.outputPlugin));
    for (const auto& o : redirectedIn)
        redirected.insert(toLower(o));
    std::map<std::pair<std::string, uint32_t>, const FormMapping*> byOutput;
    for (const auto& m : formMap)
        byOutput[{toLower(m.outputPlugin), m.objectId}] = &m;

    std::vector<std::string> lines;
    for (const auto& l : split(std::string(text), '\n', false))
        lines.push_back(l);

    // The load order the game had: "[0A] Name.esp" and "[FE:001] Name.esl".
    std::map<uint32_t, std::string> full;
    std::map<uint32_t, std::string> light;
    bool inPlugins = false;
    for (const auto& raw : lines) {
        const std::string l = trim(raw);
        if (l.rfind("Fallout 4 v", 0) == 0 && r.game.empty())
            r.game = l;
        if (l.rfind("Unhandled exception", 0) == 0 && r.exception.empty())
            r.exception = l;
        if (l == "PLUGINS:") {
            inPlugins = true;
            r.recognised = true;
            continue;
        }
        if (!inPlugins)
            continue;
        if (l.empty() || l.back() == ':') {
            if (!l.empty())
                inPlugins = false;
            continue;
        }
        if (l.front() != '[')
            continue;
        const auto close = l.find(']');
        if (close == std::string::npos)
            continue;
        const std::string idx = l.substr(1, close - 1);
        const std::string name = trim(std::string_view(l).substr(close + 1));
        try {
            if (idx.rfind("FE:", 0) == 0)
                light[static_cast<uint32_t>(std::stoul(idx.substr(3), nullptr, 16))] = name;
            else
                full[static_cast<uint32_t>(std::stoul(idx, nullptr, 16))] = name;
            ++r.pluginsListed;
            if (outputs.count(toLower(name)))
                r.outputsLoaded.push_back(name);
        } catch (const std::exception&) {
        }
    }
    r.recognised = r.recognised || !r.exception.empty();

    // Each FormID with what the log says around it (its block: the lines indented under one register or slot).
    std::set<std::string> seen;
    std::string where;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& raw = lines[i];
        const std::string l = trim(raw);
        if (!raw.empty() && raw[0] == '\t' && (raw.size() < 2 || raw[1] != '\t'))
            where = l; // "[RSP+38 ] 0x... (TESObjectREFR*)" or "RAX 0x... (...)"
        if (l.rfind("FormID:", 0) != 0)
            continue;
        const std::string value = trim(std::string_view(l).substr(7));
        uint32_t id = 0;
        try {
            id = static_cast<uint32_t>(std::stoul(value, nullptr, 16));
        } catch (const std::exception&) {
            continue;
        }
        if (!seen.insert(hex8(id)).second)
            continue;
        CrashFormRef ref;
        ref.formId = hex8(id);
        ref.where = where;
        // The other lines of the same block (same indentation), before and after.
        const size_t indent = raw.find_first_not_of('\t');
        auto sameBlock = [&](size_t j) {
            return j < lines.size() && lines[j].find_first_not_of('\t') == indent && !trim(lines[j]).empty();
        };
        auto take = [&](const std::string& t) {
            if (t.rfind("Name:", 0) == 0)
                ref.name = unquote(trim(std::string_view(t).substr(5)));
            else if (t.rfind("FormType:", 0) == 0)
                ref.type = trim(std::string_view(t).substr(9));
            else if (t.rfind("Modified by:", 0) == 0)
                ref.modifiedBy = trim(std::string_view(t).substr(12));
        };
        for (size_t j = i; j-- > 0 && sameBlock(j);)
            take(trim(lines[j]));
        for (size_t j = i + 1; sameBlock(j); ++j)
            take(trim(lines[j]));
        // The plugin, from the log's own load order.
        const uint32_t top = id >> 24;
        if (top == 0xFE) {
            const uint32_t slot = (id >> 12) & 0xFFF;
            ref.objectId = id & 0xFFF;
            if (auto it = light.find(slot); it != light.end())
                ref.plugin = it->second;
        } else if (top == 0xFF) {
            ref.plugin = "(created in game)";
            ref.objectId = id & 0xFFFFFF;
        } else {
            ref.objectId = id & 0xFFFFFF;
            if (auto it = full.find(top); it != full.end())
                ref.plugin = it->second;
        }
        const std::string key = toLower(ref.plugin);
        ref.redirected = redirected.count(key) != 0;
        if (outputs.count(key)) {
            ref.inOutput = true;
            if (auto it = byOutput.find({key, ref.objectId}); it != byOutput.end()) {
                ref.source = it->second->sourcePlugin;
                ref.sourceObjectId = it->second->sourceObjectId;
                ref.editorId = it->second->editorId;
                ref.signature = it->second->sig.str();
                ++r.translated;
            } else {
                ++r.untranslated;
            }
        } else {
            const std::string mods = toLower(ref.modifiedBy);
            for (const auto& o : outputs)
                if (mods.find(o) != std::string::npos)
                    ref.changedByOutput = true;
        }
        r.refs.push_back(std::move(ref));
    }
    return r;
}

std::string formatCrashReport(const CrashReport& r)
{
    std::string out;
    if (!r.recognised)
        return "This does not look like a Buffout 4 crash log (no PLUGINS list or exception line).\n";
    if (!r.game.empty())
        out += r.game + "\n";
    if (!r.exception.empty())
        out += r.exception + "\n";
    out += std::to_string(r.pluginsListed) + " plugins in the log's load order; " +
           std::to_string(r.outputsLoaded.size()) + " Multiplexer output plugin(s) among them" +
           (r.outputsLoaded.empty() ? std::string() : " (" + join(r.outputsLoaded, ", ") + ")") + ".\n";
    if (r.refs.empty()) {
        out += "\nThe log names no FormIDs: the crash is not tied to a record it could name. Its call stack (which "
               "module crashed) is the place to look.\n";
        return out;
    }
    out += "\nRecords the crash log names (first mention first):\n";
    for (const auto& ref : r.refs) {
        out += "\n  " + ref.formId + "  " + (ref.plugin.empty() ? std::string("(plugin not in the log's list)") : ref.plugin) +
               " " + hex6(ref.objectId);
        if (!ref.type.empty())
            out += "  " + ref.type;
        if (!ref.name.empty())
            out += "  \"" + ref.name + "\"";
        out += "\n";
        if (!ref.where.empty())
            out += "      in: " + ref.where + "\n";
        if (ref.inOutput && !ref.source.empty())
            out += "      compiled from " + ref.source + " " + hex6(ref.sourceObjectId) +
                   (ref.editorId.empty() ? std::string() : " (" + ref.editorId + ")") +
                   (ref.signature.empty() ? std::string() : " [" + ref.signature + "]") +
                   ": look at that mod first\n";
        else if (ref.inOutput)
            out += "      in an output plugin, but not in this build's FormID map: the log may come from another "
                   "build, or the record is an override (its FormID is not new)\n";
        if (ref.changedByOutput)
            out += "      changed by the compiled outputs (Modified by: " + ref.modifiedBy +
                   "): Compilation page → Generated Records shows which mods the compiled version comes from\n";
        if (ref.redirected)
            out += "      " + ref.plugin + " is a redirected copy (Master Redirection): its records are the original "
                   "plugin's, with masters pointing at the outputs\n";
    }
    out += "\n" + std::to_string(r.translated) + " FormID(s) translated to their original mod" +
           (r.untranslated ? ", " + std::to_string(r.untranslated) + " in an output but not in this build's map" : "") +
           ".\n";
    return out;
}

fs::path newestCrashLog(const fs::path& folderIn)
{
    const fs::path folder = folderIn.empty() ? platform::fallout4DocumentsDir() / "F4SE" : folderIn;
    fs::path best;
    fs::file_time_type bestTime{};
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        const std::string name = pathToUtf8(e.path().filename());
        if (!istartsWith(name, "crash-") || !iendsWith(name, ".log"))
            continue;
        const auto t = e.last_write_time(ec);
        if (best.empty() || t > bestTime) {
            best = e.path();
            bestTime = t;
        }
    }
    return best;
}

} // namespace fo4mx::diagnostics
