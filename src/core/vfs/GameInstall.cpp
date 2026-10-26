#include "vfs/GameInstall.hpp"

#include "util/Json.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fo4mx {

namespace fs = std::filesystem;

const char* installKindName(InstallKind k)
{
    switch (k) {
    case InstallKind::ModOrganizer2: return "Mod Organizer 2";
    case InstallKind::Vortex: return "Vortex";
    case InstallKind::Standalone: return "Standalone";
    }
    return "Unknown";
}

const std::vector<std::string>& basePlugins()
{
    static const std::vector<std::string> list = {
        "Fallout4.esm",       "DLCRobot.esm",       "DLCworkshop01.esm", "DLCCoast.esm",
        "DLCworkshop02.esm",  "DLCworkshop03.esm",  "DLCNukaWorld.esm",  "DLCUltraHighResolution.esm"};
    return list;
}

bool isBaseGamePlugin(std::string_view name)
{
    for (const auto& b : basePlugins())
        if (iequals(b, name))
            return true;
    return false;
}

namespace {

std::string readText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> readLines(const fs::path& p)
{
    std::vector<std::string> out;
    std::istringstream in(readText(p));
    std::string line;
    while (std::getline(in, line)) {
        auto t = trim(line);
        if (!t.empty() && t[0] != '#')
            out.push_back(t);
    }
    return out;
}

// MO2 stores paths as @ByteArray(C:\\Games\\Fallout 4) with doubled backslashes.
std::string decodeMo2Value(std::string v)
{
    v = trim(v);
    if (v.rfind("@ByteArray(", 0) == 0 && v.back() == ')')
        v = v.substr(11, v.size() - 12);
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') {
            out.push_back('\\');
            ++i;
        } else {
            out.push_back(v[i]);
        }
    }
    return out;
}

fs::path toPath(std::string s)
{
#ifndef _WIN32
    for (auto& c : s)
        if (c == '\\')
            c = '/';
#endif
    return utf8ToPath(s);
}

fs::path mo2Directory(const IniFile& ini, const fs::path& base, const char* key, const char* fallback)
{
    std::string v = decodeMo2Value(ini.get("Settings", key, ""));
    if (v.empty())
        return base / fallback;
    auto pos = v.find("%BASE_DIR%");
    if (pos != std::string::npos)
        v.replace(pos, 10, pathToUtf8(base));
    return toPath(v);
}

void loadGameInis(ResolvedInstall& out, const fs::path& dir)
{
    out.iniDirectory = dir;
    for (const char* name : {"Fallout4.ini", "Fallout4Custom.ini"}) {
        auto p = findChildCaseInsensitive(dir, name);
        if (!p.empty())
            out.ini.load(p);
    }
}

void readCreationClub(ResolvedInstall& out)
{
    auto ccc = findChildCaseInsensitive(out.gamePath, "Fallout4.ccc");
    if (!ccc.empty())
        out.creationClub = readLines(ccc);
}

void buildPluginList(ResolvedInstall& out, const std::vector<PluginListEntry>& recorded,
                     const std::vector<std::string>& loadOrder)
{
    std::vector<PluginListEntry> list;
    std::unordered_set<std::string> seen;
    auto add = [&](const PluginListEntry& e) {
        if (seen.insert(toLower(e.name)).second)
            list.push_back(e);
    };
    for (const auto& b : basePlugins())
        add({b, true, true});
    for (const auto& c : out.creationClub)
        add({c, true, true});

    std::unordered_map<std::string, bool> activeByName;
    for (const auto& e : recorded)
        activeByName[toLower(e.name)] = e.active;

    if (!loadOrder.empty()) {
        for (const auto& name : loadOrder) {
            auto it = activeByName.find(toLower(name));
            add({name, it != activeByName.end() && it->second, false});
        }
    }
    for (const auto& e : recorded)
        add(e);
    out.plugins = std::move(list);
}

ResolvedInstall resolveMo2(const InstallConfig& config)
{
    ResolvedInstall out;
    out.config = config;
    fs::path instance = config.mo2Path;
    if (fs::is_regular_file(instance))
        instance = instance.parent_path();
    auto iniPath = findChildCaseInsensitive(instance, "ModOrganizer.ini");
    if (iniPath.empty())
        throw std::runtime_error("ModOrganizer.ini not found in " + pathToUtf8(instance));

    IniFile mo2;
    mo2.load(iniPath);

    fs::path base = instance;
    std::string baseDir = decodeMo2Value(mo2.get("Settings", "base_directory", ""));
    if (!baseDir.empty())
        base = toPath(baseDir);

    out.gamePath = config.gamePath.empty() ? toPath(decodeMo2Value(mo2.get("General", "gamePath", "")))
                                           : config.gamePath;
    if (out.gamePath.empty())
        throw std::runtime_error("MO2 instance does not define gamePath");
    out.dataPath = out.gamePath / "Data";

    out.modsPath = mo2Directory(mo2, base, "mod_directory", "mods");
    out.overwritePath = mo2Directory(mo2, base, "overwrite_directory", "overwrite");
    fs::path profiles = mo2Directory(mo2, base, "profiles_directory", "profiles");

    std::string profile = config.mo2Profile;
    if (profile.empty())
        profile = decodeMo2Value(mo2.get("General", "selected_profile", "Default"));
    out.profilePath = profiles / utf8ToPath(profile);
    if (!fs::is_directory(out.profilePath))
        throw std::runtime_error("MO2 profile not found: " + pathToUtf8(out.profilePath));

    // modlist.txt lists the highest priority mod first.
    auto modLines = readLines(out.profilePath / "modlist.txt");
    std::reverse(modLines.begin(), modLines.end());
    int32_t priority = 0;
    for (const auto& line : modLines) {
        char flag = line[0];
        std::string name = line.substr(1);
        if (flag == '*')
            continue; // unmanaged (DLC / Creation Club entries)
        if (iendsWith(name, "_separator"))
            continue;
        ModInfo m;
        m.name = name;
        m.path = out.modsPath / utf8ToPath(name);
        m.priority = priority++;
        m.enabled = flag == '+';
        out.mods.push_back(std::move(m));
    }

    out.pluginsFile = config.pluginsFile.empty() ? out.profilePath / "plugins.txt" : config.pluginsFile;
    auto recorded = parsePluginsTxt(readText(out.pluginsFile));
    auto loadOrder = readLines(out.profilePath / "loadorder.txt");

    // Profile-local INIs if enabled, otherwise the global ones.
    IniFile profileSettings;
    profileSettings.load(out.profilePath / "settings.ini");
    bool localInis = iequals(profileSettings.get("General", "LocalSettings", "false"), "true");
    if (!config.iniDirectory.empty())
        loadGameInis(out, config.iniDirectory);
    else if (localInis)
        loadGameInis(out, out.profilePath);
    else
        loadGameInis(out, platform::fallout4DocumentsDir());

    readCreationClub(out);
    buildPluginList(out, recorded, loadOrder);
    return out;
}

void readVortexDeployment(ResolvedInstall& out)
{
    auto manifest = findChildCaseInsensitive(out.dataPath, "vortex.deployment.json");
    if (manifest.empty())
        return;
    try {
        auto doc = json::Value::parse(readText(manifest));
        for (const auto& f : doc["files"].asArray()) {
            auto rel = f["relPath"].asString();
            auto src = f["source"].asString();
            if (!rel.empty() && !src.empty())
                out.vortexOwners[normalizeAssetPath(rel)] = src;
        }
        spdlog::info("Vortex deployment manifest: {} files attributed to mods", out.vortexOwners.size());
    } catch (const std::exception& e) {
        out.warnings.push_back(std::string("Could not read vortex.deployment.json: ") + e.what());
    }
}

ResolvedInstall resolveDirect(const InstallConfig& config)
{
    ResolvedInstall out;
    out.config = config;
    out.gamePath = config.gamePath;
    if (out.gamePath.empty()) {
        if (auto detected = platform::detectGamePath())
            out.gamePath = *detected;
        else
            throw std::runtime_error("Fallout 4 folder not set and could not be detected");
    }
    out.dataPath = out.gamePath / "Data";
    out.pluginsFile = config.pluginsFile.empty() ? platform::fallout4LocalAppDataDir() / "plugins.txt"
                                                 : config.pluginsFile;
    auto recorded = parsePluginsTxt(readText(out.pluginsFile));
    if (recorded.empty())
        out.warnings.push_back("plugins.txt not found or empty: " + pathToUtf8(out.pluginsFile));
    loadGameInis(out, config.iniDirectory.empty() ? platform::fallout4DocumentsDir() : config.iniDirectory);
    readCreationClub(out);
    buildPluginList(out, recorded, {});
    if (config.kind == InstallKind::Vortex)
        readVortexDeployment(out);
    return out;
}

} // namespace

std::vector<PluginListEntry> parsePluginsTxt(const std::string& text)
{
    std::vector<PluginListEntry> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        auto t = trim(line);
        if (t.empty() || t[0] == '#')
            continue;
        PluginListEntry e;
        if (t[0] == '*') {
            e.active = true;
            t = t.substr(1);
        }
        e.name = t;
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<std::string> ResolvedInstall::iniArchives() const
{
    // Defaults shipped in Fallout4_Default.ini for 1.10.163.
    static const std::pair<const char*, const char*> keys[] = {
        {"sResourceIndexFileList",
         "Fallout4 - Textures1.ba2, Fallout4 - Textures2.ba2, Fallout4 - Textures3.ba2, Fallout4 - Textures4.ba2, "
         "Fallout4 - Textures5.ba2, Fallout4 - Textures6.ba2, Fallout4 - Textures7.ba2, Fallout4 - Textures8.ba2, "
         "Fallout4 - Textures9.ba2"},
        {"sResourceStartUpArchiveList",
         "Fallout4 - Startup.ba2, Fallout4 - Shaders.ba2, Fallout4 - Interface.ba2"},
        {"SResourceArchiveList",
         "Fallout4 - Voices.ba2, Fallout4 - Meshes.ba2, Fallout4 - MeshesExtra.ba2, Fallout4 - Misc.ba2, "
         "Fallout4 - Sounds.ba2, Fallout4 - Materials.ba2"},
        {"SResourceArchiveList2", "Fallout4 - Animations.ba2"},
    };
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (auto [key, def] : keys) {
        for (auto& name : split(ini.get("Archive", key, def), ',')) {
            if (!name.empty() && seen.insert(toLower(name)).second)
                out.push_back(name);
        }
    }
    return out;
}

size_t ResolvedInstall::enabledModCount() const
{
    return static_cast<size_t>(std::count_if(mods.begin(), mods.end(), [](const ModInfo& m) { return m.enabled; }));
}

ResolvedInstall resolveInstall(const InstallConfig& config)
{
    ResolvedInstall out = config.kind == InstallKind::ModOrganizer2 ? resolveMo2(config) : resolveDirect(config);
    if (!fs::is_directory(out.dataPath))
        throw std::runtime_error("Data folder not found: " + pathToUtf8(out.dataPath));
    out.gameVersion = platform::detectGameVersion(out.gamePath);
    if (config.runtime == RuntimeProfile::OldGen_1_10_163 && out.gameVersion.rfind("1.10.163", 0) != 0 &&
        out.gameVersion != "unknown")
        out.warnings.push_back("Detected Fallout 4 " + out.gameVersion +
                               "; this profile targets 1.10.163 (Old Gen). Results may not match your runtime.");
    for (const auto& w : out.warnings)
        spdlog::warn("{}", w);
    return out;
}

void populateVfs(const ResolvedInstall& install, VirtualFileSystem& vfs, ProgressSink& progress)
{
    FileSourceRoot data;
    data.name = "Data";
    data.root = install.dataPath;
    data.priority = -1;
    data.baseGame = true;
    vfs.addRoot(data);
    if (install.config.kind == InstallKind::ModOrganizer2) {
        for (const auto& m : install.mods) {
            if (!m.enabled)
                continue;
            vfs.addRoot({m.name, m.path, m.priority, false});
        }
        vfs.addRoot({"Overwrite", install.overwritePath, static_cast<int32_t>(install.mods.size()), false});
    }
    vfs.scan(progress);
}

} // namespace fo4mx
