#include "engine/OutputBackup.hpp"

#include "engine/Exporter.hpp"
#include "esp/PluginFile.hpp"
#include "util/FileTransaction.hpp"
#include "util/Text.hpp"

#include <chrono>
#include <ctime>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace fo4mx::output {

namespace fs = std::filesystem;

namespace {

std::string norm(const fs::path& p)
{
    std::string g = toLower(pathToUtf8(fs::absolute(p).lexically_normal().generic_u8string()));
    while (g.size() > 1 && g.back() == '/')
        g.pop_back();
    return g;
}

// Files and folders this app writes next to the package's Data folder (lower case).
bool isNoteName(const std::string& lower)
{
    static const char* const kNames[] = {"multiplexer output.zip", "multiplexer_build_failed.txt",
                                         "multiplexer reports", "diagnostic recorder"};
    for (const char* n : kNames)
        if (lower == n)
            return true;
    if (lower.rfind("multiplexer_", 0) == 0)
        for (const char* ext : {".txt", ".ini", ".json", ".tsv"})
            if (iendsWith(lower, ext))
                return true;
    return false;
}

// Evidence that a hand-picked output folder holds a Multiplexer export.
bool madeByMultiplexer(const fs::path& dir)
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string lower = toLower(pathToUtf8(e.path().filename()));
        if (isNoteName(lower))
            return true;
        if (e.is_regular_file(ec) && (iendsWith(lower, ".esp") || iendsWith(lower, ".esm") || iendsWith(lower, ".esl"))) {
            try {
                if (readPluginHeader(e.path()).author == kGeneratorAuthor)
                    return true;
            } catch (const std::exception&) {
            }
        }
    }
    return false;
}

} // namespace

std::string backupStamp()
{
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d%m%y %H%M%S", &tm);
    return buf;
}

bool isBackupFolderName(const std::string& name)
{
    return toLower(name).rfind(toLower(kBackupPrefix), 0) == 0;
}

bool isGameDataFolder(const fs::path& outputDirectory, const fs::path& gameData)
{
    std::error_code ec;
    if (!gameData.empty() && norm(outputDirectory) == norm(gameData))
        return true;
    return fs::exists(outputDirectory / "Fallout4.esm", ec);
}

BackupResult backupPreviousOutput(const fs::path& outputDirectory, const fs::path& gameData, const std::string& stamp)
{
    BackupResult result;
    std::error_code ec;
    const fs::path dir = fs::absolute(outputDirectory).lexically_normal();
    if (isGameDataFolder(dir, gameData)) {
        result.gameDataOutput = true;
        spdlog::warn("The output folder is the game's Data folder ({}): the previous output is not moved",
                     pathToUtf8(dir));
        return result;
    }
    fs::path root = Exporter::packageRoot(dir);
    if (root.filename().empty())
        root = root.parent_path();
    const bool package = norm(root) != norm(dir);

    // What the previous export left.
    std::vector<fs::path> entries; // paths inside `root`
    if (package) {
        if (fs::is_directory(dir, ec) && !fs::is_empty(dir, ec))
            entries.push_back(dir);
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (norm(e.path()) == norm(dir))
                continue;
            if (isNoteName(toLower(pathToUtf8(e.path().filename()))))
                entries.push_back(e.path());
        }
    } else if (fs::is_directory(dir, ec)) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string name = pathToUtf8(e.path().filename());
            if (iequals(name, "meta.ini"))
                continue; // Mod Organizer 2's description of the mod folder stays with the mod
            // Everything else moves, an older backup someone put in here too (the new backup is
            // beside the folder, never inside what is moved).
            entries.push_back(e.path());
        }
        if (!entries.empty() && !madeByMultiplexer(dir))
            throw std::runtime_error("The output folder " + pathToUtf8(dir) +
                                     " holds files that Multiplexer did not make. Choose an empty folder (or the "
                                     "folder of an earlier Multiplexer export) as the output folder; nothing was "
                                     "moved or changed.");
    }
    if (entries.empty())
        return result;

    // A new backup folder beside the output (never inside what is moved, never an existing one).
    const fs::path parent = root.parent_path();
    const std::string base = std::string(kBackupPrefix) + (stamp.empty() ? backupStamp() : stamp);
    fs::path backup = parent / utf8ToPath(base);
    for (int n = 2; fs::exists(backup, ec); ++n)
        backup = parent / utf8ToPath(base + " (" + std::to_string(n) + ")");
    if (!fs::create_directory(backup, ec) || ec)
        throw std::runtime_error("Could not create the backup folder " + pathToUtf8(backup) + ": " + ec.message() +
                                 ". The previous output was left as it is.");

    FileTransaction tx("Back up the previous output");
    for (const auto& e : entries)
        tx.rename(e, backup / e.lexically_relative(root), "previous output");
    try {
        tx.apply();
    } catch (const std::exception& e) {
        // apply() has put back whatever it had moved.
        fs::remove(backup, ec); // empty again
        throw std::runtime_error("The previous output could not be moved to " + pathToUtf8(backup) + " (" + e.what() +
                                 "). Close any program using files in " + pathToUtf8(root) +
                                 " and export again; nothing was deleted.");
    }

    // The output folder must now be empty (apart from Mod Organizer 2's meta.ini).
    fs::create_directories(dir, ec);
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = pathToUtf8(e.path().filename());
        if (!package && iequals(name, "meta.ini"))
            continue;
        tx.rollback();
        fs::remove(backup, ec);
        throw std::runtime_error("The output folder " + pathToUtf8(dir) + " is not empty after moving the previous "
                                 "output away (" + name + "); the previous output was put back.");
    }
    result.moved = true;
    result.backupFolder = backup;
    for (const auto& e : entries)
        result.entries.push_back(backup / e.lexically_relative(root));
    spdlog::info("Previous output moved to {} ({} item(s))", pathToUtf8(backup), entries.size());
    return result;
}

} // namespace fo4mx::output
