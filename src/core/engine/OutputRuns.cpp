#include "engine/OutputRuns.hpp"

#include "engine/Session.hpp"
#include "util/FileTransaction.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <thread>

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

const std::regex& runPattern()
{
    static const std::regex re("^Multiplexer Output (\\d{2})(\\d{2})(\\d{2}) (\\d{6})(?: \\((\\d+)\\))?$",
                               std::regex::icase);
    return re;
}

// Sort key of a run name: YYMMDDHHMMSS and the collision number.
std::pair<std::string, int> sortKey(const std::string& name)
{
    std::smatch m;
    if (!std::regex_match(name, m, runPattern()))
        return {"", 0};
    return {m[3].str() + m[2].str() + m[1].str() + m[4].str(), m[5].matched ? std::stoi(m[5].str()) : 1};
}

std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A finished export folder made by this app.
bool isFinishedRun(const fs::path& folder)
{
    std::error_code ec;
    return isRunName(pathToUtf8(folder.filename())) && fs::is_directory(folder, ec) &&
           fs::is_regular_file(folder / kRunMarker, ec);
}

} // namespace

bool isRunName(const std::string& name)
{
    return std::regex_match(name, runPattern());
}

std::string runStamp()
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

Location locate(const fs::path& configured, const fs::path& gamePath, const fs::path& gameData, const fs::path& modsPath)
{
    Location loc;
    std::error_code ec;
    if (configured.empty()) {
        loc.root = gamePath / "Multiplexer Output";
        return loc;
    }
    const fs::path abs = fs::absolute(configured).lexically_normal();
    if ((!gameData.empty() && norm(abs) == norm(gameData)) || fs::exists(abs / "Fallout4.esm", ec)) {
        loc.root = abs;
        loc.layout = Layout::GameData;
        return loc;
    }
    if (!modsPath.empty()) {
        const std::string mods = norm(modsPath);
        const std::string a = norm(abs);
        if (a == mods || a.rfind(mods + "/", 0) == 0) {
            loc.root = fs::absolute(modsPath).lexically_normal();
            loc.layout = Layout::ModFolder;
            return loc;
        }
    }
    fs::path root = abs;
    if (root.filename().empty())
        root = root.parent_path();
    if (iequals(pathToUtf8(root.filename()), "Data"))
        root = root.parent_path(); // the earlier package setting "...\Multiplexer Output\Data"
    loc.root = root;
    return loc;
}

Location locate(const Session& session, const fs::path& configured)
{
    return locate(configured, session.install.gamePath, session.install.dataPath, session.install.modsPath);
}

fs::path dataDir(const fs::path& runFolder, Layout layout)
{
    return layout == Layout::Package ? runFolder / "Data" : runFolder;
}

fs::path Run::dataDir() const
{
    return output::dataDir(folder, layout);
}

Run beginRun(const Location& location, const std::string& stamp)
{
    if (location.layout == Layout::GameData)
        throw std::logic_error("exports into the game's Data folder are not separate runs");
    std::error_code ec;
    fs::create_directories(location.root, ec);
    if (!fs::is_directory(location.root, ec))
        throw std::runtime_error("The output folder " + pathToUtf8(location.root) + " could not be created");
    const std::string base = std::string(kRunPrefix) + (stamp.empty() ? runStamp() : stamp);
    for (int n = 1; n < 1000; ++n) {
        const std::string name = n == 1 ? base : base + " (" + std::to_string(n) + ")";
        const fs::path finalFolder = location.root / utf8ToPath(name);
        fs::path tmp = finalFolder;
        tmp += kTmpSuffix;
        if (fs::exists(finalFolder, ec) || fs::exists(tmp, ec))
            continue; // used by another export, finished or not: never reused
        if (!fs::create_directory(tmp, ec)) {
            if (ec)
                throw std::runtime_error("The export folder " + pathToUtf8(tmp) + " could not be created: " + ec.message());
            continue; // created a moment ago by another export
        }
        Run run;
        run.name = name;
        run.folder = tmp;
        run.layout = location.layout;
        run.root = location.root;
        atomicWriteText(tmp / kRunMarker, "Fallout 4 Multiplexer Renewed export\nname=" + name + "\n");
        if (location.layout == Layout::Package)
            fs::create_directories(tmp / "Data");
        spdlog::info("Exporting into {}", pathToUtf8(tmp));
        return run;
    }
    throw std::runtime_error("No free export folder name in " + pathToUtf8(location.root));
}

void finishRun(Run& run)
{
    std::error_code ec;
    fs::path target = run.root / utf8ToPath(run.name);
    if (fs::exists(target, ec)) // made meanwhile by something else: never replaced
        for (int n = 2;; ++n) {
            const std::string name = run.name + " (" + std::to_string(n) + ")";
            target = run.root / utf8ToPath(name);
            if (!fs::exists(target, ec)) {
                run.name = name;
                break;
            }
        }
    // A virus scanner can hold a file it is checking for a moment: try again a few times.
    for (int attempt = 0;; ++attempt) {
        fs::rename(run.folder, target, ec);
        if (!ec)
            break;
        if (attempt >= 20)
            throw std::runtime_error("The export in " + pathToUtf8(run.folder) + " is complete but could not be renamed to " +
                                     pathToUtf8(target.filename()) + " (" + ec.message() +
                                     "); it is kept as it is and is not the latest export");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    run.folder = target;
    run.finished = true;
    spdlog::info("Export finished: {}", pathToUtf8(target));
}

bool setLatest(const Run& run)
{
    if (!run.finished)
        return false;
    try {
        atomicWriteText(run.root / kLatestFile, run.name + "\n");
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("Could not record the latest export in {}: {}", pathToUtf8(run.root / kLatestFile), e.what());
        return false;
    }
}

std::optional<fs::path> latestRun(const Location& location)
{
    std::error_code ec;
    if (location.layout == Layout::GameData || !fs::is_directory(location.root, ec))
        return std::nullopt;
    std::string named = slurp(location.root / kLatestFile);
    while (!named.empty() && (named.back() == '\n' || named.back() == '\r' || named.back() == ' '))
        named.pop_back();
    if (!named.empty() && isRunName(named) && isFinishedRun(location.root / utf8ToPath(named)))
        return location.root / utf8ToPath(named);
    // No (valid) pointer: the newest finished export by its name.
    std::optional<fs::path> best;
    std::pair<std::string, int> bestKey;
    for (const auto& e : fs::directory_iterator(location.root, ec)) {
        if (!isFinishedRun(e.path()))
            continue;
        const auto key = sortKey(pathToUtf8(e.path().filename()));
        if (!best || key > bestKey) {
            best = e.path();
            bestKey = key;
        }
    }
    return best;
}

CleanupResult cleanupRuns(const Location& location, size_t keep, bool removeUnfinished)
{
    CleanupResult out;
    std::error_code ec;
    if (location.layout == Layout::GameData || !fs::is_directory(location.root, ec))
        return out;
    const auto latest = latestRun(location);
    std::vector<std::pair<std::pair<std::string, int>, fs::path>> finished;
    std::vector<fs::path> unfinished;
    for (const auto& e : fs::directory_iterator(location.root, ec)) {
        const fs::path p = e.path();
        std::string name = pathToUtf8(p.filename());
        if (!e.is_directory(ec) || !fs::is_regular_file(p / kRunMarker, ec))
            continue; // not an export folder this app made
        if (iendsWith(name, kTmpSuffix)) {
            if (isRunName(name.substr(0, name.size() - std::string(kTmpSuffix).size())))
                unfinished.push_back(p);
        } else if (isRunName(name)) {
            finished.emplace_back(sortKey(name), p);
        }
    }
    std::sort(finished.rbegin(), finished.rend()); // newest first
    std::vector<fs::path> doomed;
    for (size_t i = 0; i < finished.size(); ++i)
        if (i >= keep && !(latest && norm(*latest) == norm(finished[i].second)))
            doomed.push_back(finished[i].second);
    if (removeUnfinished)
        doomed.insert(doomed.end(), unfinished.begin(), unfinished.end());
    for (const auto& p : doomed) {
        // Renamed first: a folder with a file in use cannot be, and stays untouched.
        fs::path deleting = p;
        deleting += ".deleting";
        fs::rename(p, deleting, ec);
        if (ec) {
            out.skipped.emplace_back(p, ec.message());
            spdlog::warn("Kept {}: {}", pathToUtf8(p), ec.message());
            continue;
        }
        fs::remove_all(deleting, ec);
        if (ec) {
            out.skipped.emplace_back(p, "partly deleted (" + ec.message() + "); left as " +
                                            pathToUtf8(deleting.filename()));
            continue;
        }
        out.removed.push_back(p);
        spdlog::info("Removed the old export {}", pathToUtf8(p));
    }
    return out;
}

} // namespace fo4mx::output
