#include "engine/PluginIni.hpp"

#include "util/Text.hpp"

#include <fstream>
#include <sstream>

namespace fo4mx::plugin_ini {

namespace {

std::string trimmed(std::string s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

using Key = std::pair<std::string, std::string>; // lowercase section, key

} // namespace

std::filesystem::path fileOf(const Session& session, PluginIndex plugin)
{
    const std::string want = toLower(session.plugins[plugin].baseName()) + ".ini";
    for (const auto* f : session.vfs.rootFiles())
        if (toLower(f->relativePath) == want)
            return session.vfs.winnerPath(*f);
    return {};
}

std::vector<Entry> read(const std::filesystem::path& file)
{
    std::vector<Entry> out;
    std::ifstream in(file, std::ios::binary);
    std::string line, section;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {
            first = false;
            if (line.size() >= 3 && static_cast<uint8_t>(line[0]) == 0xEF && static_cast<uint8_t>(line[1]) == 0xBB &&
                static_cast<uint8_t>(line[2]) == 0xBF)
                line.erase(0, 3);
        }
        const std::string t = trimmed(line);
        if (t.empty() || t[0] == ';' || t[0] == '#')
            continue;
        if (t.front() == '[' && t.back() == ']') {
            section = trimmed(t.substr(1, t.size() - 2));
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos || section.empty()) // a setting outside any section is not read
            continue;
        out.push_back({section, trimmed(t.substr(0, eq)), trimmed(t.substr(eq + 1))});
    }
    return out;
}

std::map<PluginIndex, std::string> conflicts(const Session& session, const std::vector<uint8_t>& compiled)
{
    // Each active plugin's settings, in load order.
    std::vector<std::pair<PluginIndex, std::vector<Entry>>> inis;
    for (auto pi : session.loadOrder) {
        const auto file = fileOf(session, pi);
        if (!file.empty())
            inis.emplace_back(pi, read(file));
    }
    auto isCompiled = [&](PluginIndex p) { return p < compiled.size() && compiled[p]; };
    struct Final {
        std::string value;
        PluginIndex from = kInvalidPlugin;
    };
    std::map<Key, Final> today, after;
    for (const auto& [pi, entries] : inis)
        for (const auto& e : entries)
            today[{toLower(e.section), toLower(e.key)}] = {e.value, pi};
    for (bool compiledPass : {false, true}) // plugins that stay, then the outputs' INI (loaded last)
        for (const auto& [pi, entries] : inis)
            if (isCompiled(pi) == compiledPass)
                for (const auto& e : entries)
                    after[{toLower(e.section), toLower(e.key)}] = {e.value, pi};
    std::map<PluginIndex, std::string> out;
    // Archive settings: the build lists compiled plugins' archives itself (Fallout4Custom.ini, or the archive
    // loader), and a plugin INI read after it could replace that list. Such a plugin stays.
    for (const auto& [pi, entries] : inis)
        if (isCompiled(pi))
            for (const auto& e : entries)
                if (toLower(e.section) == "archive") {
                    out[pi] = "its INI (" + session.plugins[pi].baseName() + ".ini) sets [" + e.section + "] " + e.key +
                              ": archive settings are managed by the build (the compiled plugins' archives are listed "
                              "for them), and this INI, read after it, could replace that list";
                    break;
                }
    for (const auto& [key, now] : today) {
        const auto it = after.find(key);
        if (it != after.end() && it->second.value == now.value)
            continue;
        // A compiled plugin setting it would change the final value: it stays.
        for (const auto& [pi, entries] : inis) {
            if (!isCompiled(pi) || out.count(pi))
                continue;
            for (const auto& e : entries)
                if (toLower(e.section) == key.first && toLower(e.key) == key.second) {
                    out[pi] = "its INI (" + session.plugins[pi].baseName() + ".ini) sets [" + e.section + "] " + e.key +
                              ", which " + session.pluginName(now.from) + " sets to " + now.value +
                              " later in the load order today; the outputs load after every plugin that stays, so "
                              "compiled the setting would end with another value";
                    break;
                }
        }
    }
    return out;
}

std::string merged(const Session& session, const std::vector<PluginIndex>& compiled, std::vector<std::string>* from)
{
    std::vector<uint8_t> flag(session.plugins.size(), 0);
    for (auto p : compiled)
        if (p < flag.size())
            flag[p] = 1;
    std::ostringstream out;
    bool any = false;
    for (auto pi : session.loadOrder) {
        if (!flag[pi])
            continue;
        const auto file = fileOf(session, pi);
        if (file.empty())
            continue;
        const auto entries = read(file);
        if (!any)
            out << kTag << ": the settings of compiled plugins' own INI files, in load order.\n"
                << "; The game reads this file while this output plugin is loaded.\n";
        any = true;
        if (from)
            from->push_back(session.plugins[pi].baseName() + ".ini");
        out << "\n; From " << session.plugins[pi].baseName() << ".ini\n";
        std::string section = "\x01";
        for (const auto& e : entries) {
            if (e.section != section) {
                section = e.section;
                out << "[" << section << "]\n";
            }
            out << e.key << "=" << e.value << "\n";
        }
    }
    return any ? out.str() : std::string();
}

} // namespace fo4mx::plugin_ini
