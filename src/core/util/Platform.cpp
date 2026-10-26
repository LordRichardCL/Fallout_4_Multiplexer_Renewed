#include "util/Platform.hpp"

#include "util/Text.hpp"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace fo4mx::platform {

namespace {

std::filesystem::path envPath(const char* name)
{
#ifdef _WIN32
    wchar_t buf[32768];
    std::wstring wname(name, name + std::strlen(name));
    DWORD n = GetEnvironmentVariableW(wname.c_str(), buf, 32768);
    if (n == 0 || n >= 32768)
        return {};
    return std::filesystem::path(std::wstring(buf, n));
#else
    const char* v = std::getenv(name);
    return v ? std::filesystem::path(v) : std::filesystem::path();
#endif
}

} // namespace

std::filesystem::path fallout4DocumentsDir()
{
#ifdef _WIN32
    PWSTR raw = nullptr;
    std::filesystem::path docs;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw)))
        docs = raw;
    CoTaskMemFree(raw);
    if (!docs.empty())
        return docs / "My Games" / "Fallout4";
#endif
    auto home = envPath("HOME");
    return home.empty() ? std::filesystem::path() : home / "Documents" / "My Games" / "Fallout4";
}

std::filesystem::path localAppDataDir()
{
    auto p = envPath("LOCALAPPDATA");
    if (!p.empty())
        return p;
    auto home = envPath("HOME");
    return home.empty() ? std::filesystem::path() : home / ".local" / "share";
}

std::filesystem::path fallout4LocalAppDataDir()
{
    auto base = localAppDataDir();
    return base.empty() ? base : base / "Fallout4";
}

std::filesystem::path applicationDataDir()
{
    auto base = localAppDataDir();
    if (base.empty())
        base = std::filesystem::temp_directory_path();
    return base / "Fallout4MultiplexerRenewed";
}

std::optional<std::filesystem::path> detectGamePath()
{
#ifdef _WIN32
    const wchar_t* keys[] = {L"SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Fallout4",
                             L"SOFTWARE\\Bethesda Softworks\\Fallout4",
                             L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1998527297"};
    const wchar_t* values[] = {L"Installed Path", L"Installed Path", L"path"};
    for (int i = 0; i < 3; ++i) {
        wchar_t buf[MAX_PATH * 2];
        DWORD size = sizeof(buf);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, keys[i], values[i], RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
            std::filesystem::path p(buf);
            if (std::filesystem::exists(p / "Fallout4.exe"))
                return p;
        }
    }
#endif
    return std::nullopt;
}

std::string detectGameVersion(const std::filesystem::path& gamePath)
{
#ifdef _WIN32
    auto exe = (gamePath / "Fallout4.exe").wstring();
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
    if (size > 0) {
        std::string buffer(size, '\0');
        if (GetFileVersionInfoW(exe.c_str(), 0, size, buffer.data())) {
            VS_FIXEDFILEINFO* info = nullptr;
            UINT len = 0;
            if (VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &len) && info) {
                return std::to_string(HIWORD(info->dwFileVersionMS)) + "." +
                       std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
                       std::to_string(HIWORD(info->dwFileVersionLS)) + "." +
                       std::to_string(LOWORD(info->dwFileVersionLS));
            }
        }
    }
#endif
    // Fall back to the F4SE runtime DLL, which is named after the game version.
    std::error_code ec;
    if (!gamePath.empty()) {
        for (const auto& entry : std::filesystem::directory_iterator(gamePath, ec)) {
            auto name = toLower(pathToUtf8(entry.path().filename()));
            if (name.rfind("f4se_", 0) == 0 && name.size() > 9 && iendsWith(name, ".dll") &&
                name.find("loader") == std::string::npos && name.find("steam") == std::string::npos) {
                auto ver = name.substr(5, name.size() - 9);
                for (auto& c : ver)
                    if (c == '_')
                        c = '.';
                return ver + " (from F4SE)";
            }
        }
    }
    return "unknown";
}

} // namespace fo4mx::platform
