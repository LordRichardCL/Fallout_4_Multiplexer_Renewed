#include "engine/VortexSupport.hpp"

#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <fstream>

namespace fo4mx::vortex {

namespace fs = std::filesystem;

fs::path suggestedDownloadsFolder()
{
    const auto base = platform::roamingAppDataDir();
    if (base.empty())
        return {};
    const auto folder = base / "Vortex" / "downloads" / kGameId;
    std::error_code ec;
    return fs::is_directory(folder, ec) ? folder : fs::path();
}

FolderCheck checkDownloadsFolder(const fs::path& folder)
{
    FolderCheck c;
    std::error_code ec;
    if (folder.empty()) {
        c.problems.push_back("no folder is set");
        return c;
    }
    if (!fs::exists(folder, ec)) {
        c.problems.push_back("the folder does not exist: " + pathToUtf8(folder));
        return c;
    }
    if (!fs::is_directory(folder, ec)) {
        c.problems.push_back("not a folder: " + pathToUtf8(folder));
        return c;
    }
    // Writable: create and remove a probe file.
    const auto probe = folder / ".multiplexer-write-test";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out || !(out << "x"))
            c.problems.push_back("this app cannot write to " + pathToUtf8(folder));
    }
    fs::remove(probe, ec);
    if (!iequals(pathToUtf8(folder.filename()), kGameId))
        c.notes.push_back("Vortex keeps Fallout 4 downloads in a folder named \"" + std::string(kGameId) +
                          "\" inside its downloads folder; this folder is named \"" + pathToUtf8(folder.filename()) +
                          "\", so Vortex may not list the archive");
    return c;
}

fs::path uniqueName(const fs::path& folder, const std::string& fileName)
{
    std::error_code ec;
    if (!fs::exists(folder / utf8ToPath(fileName), ec))
        return folder / utf8ToPath(fileName);
    const fs::path name = utf8ToPath(fileName);
    const std::string stem = pathToUtf8(name.stem());
    const std::string ext = pathToUtf8(name.extension());
    for (int n = 2; n < 10000; ++n) {
        auto candidate = folder / utf8ToPath(stem + " (" + std::to_string(n) + ")" + ext);
        if (!fs::exists(candidate, ec))
            return candidate;
    }
    throw std::runtime_error("no free name for " + fileName + " in " + pathToUtf8(folder));
}

} // namespace fo4mx::vortex
