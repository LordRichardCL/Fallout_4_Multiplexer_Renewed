#pragma once

#include "esp/RecordDefinitions.hpp"
#include "util/IniFile.hpp"
#include "vfs/VirtualFileSystem.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fo4mx {

enum class InstallKind : uint8_t { ModOrganizer2, Vortex, Standalone };
const char* installKindName(InstallKind k);

struct InstallConfig {
    InstallKind kind = InstallKind::Standalone;
    std::filesystem::path gamePath;     // Fallout 4 folder (contains Fallout4.exe and Data)
    std::filesystem::path mo2Path;      // MO2 instance folder (contains ModOrganizer.ini)
    std::string mo2Profile;             // empty = MO2's selected profile
    std::filesystem::path pluginsFile;  // optional plugins.txt override
    std::filesystem::path iniDirectory; // optional "My Games/Fallout4" override
    std::string language = "en";
    RuntimeProfile runtime = RuntimeProfile::OldGen_1_10_163;
};

struct ModInfo {
    std::string name;
    std::filesystem::path path;
    int32_t priority = 0;
    bool enabled = true;
};

struct PluginListEntry {
    std::string name;
    bool active = false;
    bool implicit = false; // always loaded by the engine (base game, DLC, Creation Club)
};

struct ResolvedInstall {
    InstallConfig config;
    std::filesystem::path gamePath;
    std::filesystem::path dataPath;
    std::filesystem::path profilePath;   // MO2 profile folder
    std::filesystem::path modsPath;      // MO2 mods folder
    std::filesystem::path overwritePath; // MO2 overwrite folder
    std::filesystem::path pluginsFile;   // plugins.txt actually used
    std::filesystem::path iniDirectory;  // folder whose Fallout4*.ini were read
    std::vector<ModInfo> mods;           // enabled + disabled, ascending priority
    std::vector<PluginListEntry> plugins; // recorded load order (implicit plugins first)
    std::vector<std::string> creationClub;
    IniFile ini;
    std::string gameVersion;
    std::unordered_map<std::string, std::string> vortexOwners; // normalised path -> Vortex mod
    std::vector<std::string> warnings;

    // Archives the engine loads from INI settings, in load order.
    std::vector<std::string> iniArchives() const;
    size_t enabledModCount() const;
};

// Names of the plugins that are always loaded by Fallout 4 (base game + DLC).
const std::vector<std::string>& basePlugins();
bool isBaseGamePlugin(std::string_view name);

// Reads the mod manager / game configuration. Throws on unusable configuration.
ResolvedInstall resolveInstall(const InstallConfig& config);

// Adds the Data folder and (for MO2) all enabled mods + overwrite to the VFS, then scans.
void populateVfs(const ResolvedInstall& install, VirtualFileSystem& vfs, ProgressSink& progress = nullProgress());

// Parses a plugins.txt (asterisk format). Lines without '*' are inactive.
std::vector<PluginListEntry> parsePluginsTxt(const std::string& text);

} // namespace fo4mx
