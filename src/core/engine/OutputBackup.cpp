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

namespace {

// Splits `entry` so that nothing in `keep` is moved: a kept file stays, a folder holding one is
// entered and its other contents are taken one by one.
void collect(const fs::path& entry, const std::vector<std::string>& keep, std::vector<fs::path>& out)
{
    const std::string e = norm(entry);
    for (const auto& k : keep)
        if (k == e)
            return; // this app's own file: stays where it is
    bool holdsKept = false;
    for (const auto& k : keep)
        holdsKept |= k.size() > e.size() && k.compare(0, e.size(), e) == 0 && k[e.size()] == '/';
    std::error_code ec;
    if (holdsKept && fs::is_directory(entry, ec)) {
        for (const auto& c : fs::directory_iterator(entry, ec))
            collect(c.path(), keep, out);
        return;
    }
    out.push_back(entry);
}

bool isKept(const fs::path& p, const std::vector<std::string>& keep)
{
    const std::string e = norm(p);
    for (const auto& k : keep)
        if (k == e || (k.size() > e.size() && k.compare(0, e.size(), e) == 0 && k[e.size()] == '/'))
            return true; // the file itself, or a folder on the way to it
    return false;
}

// Removes the folders under (and including) `dir` that are empty; never a file.
void removeEmptyFolders(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return;
    for (const auto& c : fs::directory_iterator(dir, ec))
        if (c.is_directory(ec))
            removeEmptyFolders(c.path());
    fs::remove(dir, ec); // fails, harmlessly, when something is still in it
}

} // namespace

BackupResult backupPreviousOutput(const fs::path& outputDirectory, const fs::path& gameData, const std::string& stamp,
                                  const std::vector<fs::path>& keepFiles)
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

    // This app's own working files (its database and build cache, open while it runs) never move.
    std::vector<std::string> keep;
    for (const auto& k : keepFiles)
        if (!k.empty())
            keep.push_back(norm(k));
    for (const auto& k : keepFiles)
        if (!k.empty() && norm(k).rfind(norm(root) + "/", 0) == 0 && fs::exists(k, ec))
            result.keptInPlace.push_back(k);

    // What the previous export left.
    std::vector<fs::path> entries; // paths inside `root`
    if (package) {
        if (fs::is_directory(dir, ec) && !fs::is_empty(dir, ec))
            collect(dir, keep, entries);
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (norm(e.path()) == norm(dir))
                continue;
            if (isNoteName(toLower(pathToUtf8(e.path().filename()))))
                collect(e.path(), keep, entries);
        }
    } else if (fs::is_directory(dir, ec)) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string name = pathToUtf8(e.path().filename());
            if (iequals(name, "meta.ini"))
                continue; // Mod Organizer 2's description of the mod folder stays with the mod
            // Everything else moves, an older backup someone put in here too (the new backup is
            // beside the folder, never inside what is moved).
            collect(e.path(), keep, entries);
        }
        if (!entries.empty() && !madeByMultiplexer(dir))
            throw std::runtime_error("The output folder " + pathToUtf8(dir) +
                                     " holds files that Multiplexer did not make. Choose an empty folder (or the "
                                     "folder of an earlier Multiplexer export) as the output folder; nothing was "
                                     "moved or changed.");
    }
    for (const auto& k : result.keptInPlace)
        spdlog::warn("{} is Multiplexer's own working file inside the output folder: it stays where it is and is "
                     "never packaged. Keeping the results database elsewhere (Settings) is better.",
                     pathToUtf8(k));
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
    for (const auto& e : entries) {
        const fs::path target = backup / e.lexically_relative(root);
        fs::create_directories(target.parent_path(), ec); // a folder split around a kept file
        tx.rename(e, target, "previous output");
    }
    try {
        tx.apply();
    } catch (const std::exception& e) {
        // apply() has put back whatever it had moved; only empty folders are left in the backup.
        removeEmptyFolders(backup);
        throw std::runtime_error(
            "The previous output could not be moved to " + pathToUtf8(backup) + " (" + e.what() +
            "). A program has a file in " + pathToUtf8(root) +
            " open (File Explorer showing it, an editor, antivirus, Mod Organizer 2, the game): close it and export "
            "again. Nothing was moved or deleted.");
    }

    // The output folder must now be empty, apart from Mod Organizer 2's meta.ini and this app's own
    // working files.
    fs::create_directories(dir, ec);
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec)
            break;
        const fs::path p = it->path();
        if (isKept(p, keep))
            continue;
        if (!package && p.parent_path() == dir && iequals(pathToUtf8(p.filename()), "meta.ini"))
            continue;
        if (it->is_directory(ec) && fs::is_empty(p, ec))
            continue; // an empty folder left by a split
        const std::string name = pathToUtf8(p.lexically_relative(dir));
        tx.rollback();
        removeEmptyFolders(backup);
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
