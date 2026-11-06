#include "engine/Mo2Support.hpp"

#include "engine/Session.hpp"
#include "util/FileTransaction.hpp"
#include "util/Text.hpp"

#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

namespace fo4mx::mo2 {

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string norm(const fs::path& p)
{
    std::string g = toLower(pathToUtf8(p.lexically_normal().generic_u8string()));
    while (g.size() > 1 && g.back() == '/')
        g.pop_back();
    return g;
}

bool sameFile(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    if (!fs::is_regular_file(a, ec) || !fs::is_regular_file(b, ec) || fs::file_size(a, ec) != fs::file_size(b, ec))
        return false;
    return slurp(a) == slurp(b);
}

} // namespace

std::string outputModName(const Session& session, const fs::path& outputDirectory)
{
    const auto& mods = session.install.modsPath;
    if (mods.empty() || outputDirectory.empty())
        return {};
    if (norm(outputDirectory.parent_path()) != norm(mods))
        return {};
    return pathToUtf8(outputDirectory.lexically_normal().filename());
}

std::vector<ModListLine> readModList(const fs::path& profilePath)
{
    std::vector<ModListLine> out;
    std::istringstream in(slurp(profilePath / "modlist.txt"));
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() < 2 || line[0] == '#')
            continue;
        out.push_back({line[0], line.substr(1)});
    }
    return out;
}

bool modEnabled(const fs::path& profilePath, const std::string& modName)
{
    for (const auto& l : readModList(profilePath))
        if (l.flag == '+' && iequals(l.name, modName))
            return true;
    return false;
}

std::vector<std::string> outputsNotVisible(const Session& session, const std::vector<fs::path>& outputs,
                                           const std::string& assumeEnabled)
{
    // Where the game sees files through MO2: enabled mods (highest priority first), overwrite,
    // then the game's Data folder. The highest-priority copy is the one the game loads.
    std::vector<fs::path> roots;
    roots.push_back(session.install.overwritePath);
    for (const auto& l : readModList(session.install.profilePath))
        if ((l.flag == '+' || iequals(l.name, assumeEnabled)) && !iendsWith(l.name, "_separator"))
            roots.push_back(session.install.modsPath / utf8ToPath(l.name));
    if (!assumeEnabled.empty()) {
        bool listed = false;
        for (const auto& l : readModList(session.install.profilePath))
            listed |= iequals(l.name, assumeEnabled);
        if (!listed) // it is added at the top
            roots.insert(roots.begin() + 1, session.install.modsPath / utf8ToPath(assumeEnabled));
    }
    roots.push_back(session.install.dataPath);

    std::vector<std::string> missing;
    std::error_code ec;
    for (const auto& f : outputs) {
        bool ok = false;
        for (const auto& root : roots) {
            if (root.empty())
                continue;
            const auto candidate = root / f.filename();
            if (fs::is_regular_file(candidate, ec)) {
                ok = sameFile(candidate, f); // the winner decides
                break;
            }
        }
        if (!ok)
            missing.push_back(pathToUtf8(f.filename()));
    }
    return missing;
}

void planEnableMod(FileTransaction& tx, const fs::path& profilePath, const std::string& modName)
{
    const auto file = profilePath / "modlist.txt";
    const std::string text = slurp(file);
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::istringstream in(text);
    std::string out;
    bool found = false;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() > 1 && (line[0] == '-' || line[0] == '+') && iequals(line.substr(1), modName)) {
            line[0] = '+';
            found = true;
        }
        out += line + eol;
    }
    if (!found) {
        // After MO2's comment header, before the first mod: the highest priority.
        std::string header, rest;
        std::istringstream again(out);
        bool inHeader = true;
        for (std::string line; std::getline(again, line);) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (inHeader && !line.empty() && line[0] == '#')
                header += line + eol;
            else {
                inHeader = false;
                rest += line + eol;
            }
        }
        out = header + "+" + modName + eol + rest;
    }
    tx.write(file, out, "enable the mod \"" + modName + "\" (highest priority when it is new)");
}

Ownership ownership(const Session& session, const std::vector<std::string>& compiledPlugins)
{
    Ownership o;
    std::map<std::string, std::vector<std::string>> byMod;
    for (const auto& name : compiledPlugins) {
        const auto* p = session.findPlugin(name);
        if (!p)
            continue;
        ++o.plugins;
        byMod[p->sourceMod.empty() ? std::string("(game Data folder)") : p->sourceMod].push_back(p->name);
        if (const auto* f = session.vfs.find(normalizeAssetPath(p->name)); f && f->sources.size() > 1)
            ++o.providedTwice;
    }
    o.mods = byMod.size();
    for (const auto& [mod, plugins] : byMod) {
        if (o.lines.size() >= 30) {
            o.lines.push_back("... and " + std::to_string(byMod.size() - 30) + " more mods");
            break;
        }
        o.lines.push_back(mod + ": " + join(plugins, ", "));
    }
    return o;
}

} // namespace fo4mx::mo2
