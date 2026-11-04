// MultiplexerDiagnosticRecorder: an optional F4SE plugin that records a small, fixed set of
// diagnostic events into a JSON Lines file Fallout 4 Multiplexer Renewed reads
// (docs/DIAGNOSTIC_RECORDER.md describes the format).
//
// What it records: its own start and the versions involved, the F4SE lifecycle messages (plugins
// loaded, game data ready, new game, save loading and saving), the active plugins listed in
// plugins.txt and Fallout4.ccc as the game process reads them, the F4SE plugin DLLs present and
// which of them are loaded, F4SE's own plugin load results (f4se.log), and its own errors.
//
// What it does not do: it uses no game addresses, reads no game structures, installs no hooks,
// changes no game data, save or file other than its own log. Every entry point catches every
// exception, so a failure of the recorder is written to the log instead of reaching the game.

#include "F4SEPluginAbi.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kRecorderVersion = f4se_abi::packVersion(1, 0, 0);
constexpr const char* kRecorderVersionText = "1.0.0";
constexpr int kSchema = 1;
constexpr size_t kKeepSessions = 10;   // older session logs are deleted
constexpr size_t kMaxF4seLines = 400;  // plugin lines copied from f4se.log
constexpr size_t kMaxPlugins = 8192;   // entries copied from plugins.txt

std::mutex g_lock;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::uint64_t g_startTick = 0;
std::uint64_t g_seq = 0;
f4se_abi::PluginHandle g_handle = f4se_abi::kPluginHandleInvalid;
std::uint32_t g_f4seVersion = 0;
std::uint32_t g_runtimeVersion = 0;
std::wstring g_gameDir;    // folder of Fallout4.exe, with a trailing backslash
std::wstring g_myGames;    // Documents\My Games\<save folder>\, with a trailing backslash
std::wstring g_userProfile; // replaced by %USERPROFILE% in recorded paths
bool g_loadRecorded = false;

// ---- Text ----------------------------------------------------------------------------------

std::string utf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring lower(std::wstring s)
{
    for (auto& c : s)
        c = static_cast<wchar_t>(towlower(c));
    return s;
}

// A recorded path: the user's profile folder (which holds their account name) is replaced.
std::string pathText(const std::wstring& path)
{
    if (!g_userProfile.empty() && path.size() >= g_userProfile.size() &&
        lower(path.substr(0, g_userProfile.size())) == lower(g_userProfile))
        return "%USERPROFILE%" + utf8(path.substr(g_userProfile.size()));
    return utf8(path);
}

std::string jsonString(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

std::string versionText(std::uint32_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 4) & 0xFFF, v & 0xF);
    return buf;
}

// One event: {"schema":1,"seq":n,"time":"...","ms":n,"event":"...", fields...}
class Event {
public:
    explicit Event(const char* name) : name_(name) {}
    Event& str(const char* key, const std::string& v) { return raw(key, jsonString(v)); }
    Event& num(const char* key, std::uint64_t v) { return raw(key, std::to_string(v)); }
    Event& flag(const char* key, bool v) { return raw(key, v ? "true" : "false"); }
    Event& raw(const char* key, const std::string& json)
    {
        fields_ += ",";
        fields_ += jsonString(key);
        fields_ += ":";
        fields_ += json;
        return *this;
    }
    void write();

private:
    std::string name_;
    std::string fields_;
};

std::string utcNow()
{
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond, t.wMilliseconds);
    return buf;
}

void writeLine(const std::string& line)
{
    if (g_file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    FlushFileBuffers(g_file); // events are few: each one is on disk before the game goes on
}

void Event::write()
{
    std::lock_guard<std::mutex> lock(g_lock);
    std::string line = "{\"schema\":" + std::to_string(kSchema) + ",\"seq\":" + std::to_string(g_seq++) +
                       ",\"time\":" + jsonString(utcNow()) +
                       ",\"ms\":" + std::to_string(GetTickCount64() - g_startTick) + ",\"event\":" + jsonString(name_) +
                       fields_ + "}\n";
    writeLine(line);
}

void recorderError(const std::string& where, const std::string& message)
{
    Event("recorder_error").str("where", where).str("message", message).write();
}

void recorderWarning(const std::string& where, const std::string& message)
{
    Event("recorder_warning").str("where", where).str("message", message).write();
}

// ---- Files and folders ---------------------------------------------------------------------

std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && path)
        out = path;
    if (path)
        CoTaskMemFree(path);
    if (!out.empty() && out.back() != L'\\')
        out += L'\\';
    return out;
}

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The whole file, read while other programs (F4SE) may still be writing it.
bool readShared(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = out.empty() || ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    out.resize(ok ? read : 0);
    CloseHandle(h);
    return ok;
}

std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos)
            nl = text.size();
        std::string line = text.substr(at, nl - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        out.push_back(std::move(line));
        at = nl + 1;
    }
    return out;
}

std::vector<std::wstring> filesIn(const std::wstring& folder, const wchar_t* pattern)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::string fileVersion(const std::wstring& path)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0)
        return {};
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info)
        return {};
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                  HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return buf;
}

// ---- Session file --------------------------------------------------------------------------

std::wstring logFolder()
{
    return g_myGames.empty() ? std::wstring() : g_myGames + L"F4SE\\MultiplexerDiagnosticRecorder\\";
}

void openSession()
{
    const std::wstring folder = logFolder();
    if (folder.empty())
        return;
    CreateDirectoryW((g_myGames + L"F4SE").c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    // Keep the newest sessions (names sort by their start time).
    auto old = filesIn(folder, L"session-*.jsonl");
    while (old.size() >= kKeepSessions) {
        DeleteFileW((folder + old.front()).c_str());
        old.erase(old.begin());
    }
    SYSTEMTIME t;
    GetSystemTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"session-%04u%02u%02u-%02u%02u%02u-%u.jsonl", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, GetCurrentProcessId());
    g_file = CreateFileW((folder + name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

// ---- Snapshots -----------------------------------------------------------------------------

// The plugins the game reads as active: plugins.txt (as the game process sees it, so Mod
// Organizer 2's profile file under its virtual file system) and Fallout4.ccc.
void recordLoadOrder()
{
    const std::wstring pluginsTxt = knownFolder(FOLDERID_LocalAppData) + L"Fallout4\\plugins.txt";
    std::string text;
    if (!readShared(pluginsTxt, text)) {
        recorderWarning("load_order", "plugins.txt could not be read: " + pathText(pluginsTxt));
    } else {
        std::string entries = "[";
        size_t count = 0, active = 0;
        for (const auto& l : lines(text)) {
            if (l.empty() || l[0] == '#')
                continue;
            if (count >= kMaxPlugins)
                break;
            const bool on = l[0] == '*';
            const std::string name = on ? l.substr(1) : l;
            if (count++)
                entries += ",";
            entries += "{\"name\":" + jsonString(name) + ",\"active\":" + (on ? "true" : "false") + "}";
            active += on;
        }
        entries += "]";
        Event("load_order")
            .str("source", "plugins.txt")
            .str("path", pathText(pluginsTxt))
            .num("entries", count)
            .num("active", active)
            .raw("plugins", entries)
            .write();
    }
    std::string ccc;
    if (readShared(g_gameDir + L"Fallout4.ccc", ccc)) {
        std::string entries = "[";
        size_t count = 0;
        for (const auto& l : lines(ccc)) {
            if (l.empty())
                continue;
            const std::wstring wide(l.begin(), l.end()); // Creation Club names are ASCII
            if (count++)
                entries += ",";
            entries += "{\"name\":" + jsonString(l) + ",\"present\":" +
                       (exists(g_gameDir + L"Data\\" + wide) ? "true" : "false") + "}";
        }
        entries += "]";
        Event("creation_club").str("path", pathText(g_gameDir + L"Fallout4.ccc")).raw("plugins", entries).write();
    }
}

// F4SE plugin DLLs present, the modules loaded from the game folder or among them, and F4SE's own
// plugin load results.
void recordModules()
{
    const std::wstring pluginDir = g_gameDir + L"Data\\F4SE\\Plugins\\";
    const auto dlls = filesIn(pluginDir, L"*.dll");

    std::vector<HMODULE> modules(1024);
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                               &needed)) {
        recorderError("modules", "the process's modules could not be listed (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    if (needed > modules.size() * sizeof(HMODULE)) {
        modules.resize(needed / sizeof(HMODULE));
        K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                              &needed);
    }
    modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));

    std::vector<std::wstring> loadedNames;
    std::string list = "[";
    size_t listed = 0, others = 0;
    const std::wstring gameDir = lower(g_gameDir);
    for (HMODULE m : modules) {
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(m, buf, MAX_PATH * 2);
        if (n == 0)
            continue;
        const std::wstring path(buf, n);
        const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
        loadedNames.push_back(lower(name));
        const bool inGame = lower(path).rfind(gameDir, 0) == 0;
        const bool f4sePlugin = std::any_of(dlls.begin(), dlls.end(), [&](const std::wstring& d) { return lower(d) == lower(name); });
        if (!inGame && !f4sePlugin) {
            ++others; // system and driver modules: counted only
            continue;
        }
        MODULEINFO info{};
        K32GetModuleInformation(GetCurrentProcess(), m, &info, sizeof(info));
        if (listed++)
            list += ",";
        list += "{\"name\":" + jsonString(utf8(name)) + ",\"path\":" + jsonString(pathText(path)) +
                ",\"size\":" + std::to_string(info.SizeOfImage) + ",\"version\":" + jsonString(fileVersion(path)) +
                ",\"f4se_plugin\":" + (f4sePlugin ? "true" : "false") + "}";
    }
    list += "]";
    Event("modules").raw("modules", list).num("other_modules", others).write();

    std::string present = "[";
    size_t count = 0;
    for (const auto& d : dlls) {
        const bool loaded = std::find(loadedNames.begin(), loadedNames.end(), lower(d)) != loadedNames.end();
        if (count++)
            present += ",";
        present += "{\"name\":" + jsonString(utf8(d)) + ",\"loaded\":" + (loaded ? "true" : "false") + "}";
    }
    present += "]";
    // F4SE's Address Library data for this runtime (most F4SE plugins need it).
    char bin[64];
    std::snprintf(bin, sizeof(bin), "version-%u-%u-%u-0.bin", (g_runtimeVersion >> 24) & 0xFF,
                  (g_runtimeVersion >> 16) & 0xFF, (g_runtimeVersion >> 4) & 0xFFF);
    const std::wstring binWide(bin, bin + std::strlen(bin));
    Event("f4se_plugins")
        .str("folder", pathText(pluginDir))
        .raw("dlls", present)
        .str("address_library_file", bin)
        .flag("address_library_present", exists(pluginDir + binWide))
        .write();

    // F4SE's log: one line per plugin with its load result ("loaded correctly", or why not).
    const std::wstring f4seLog = g_myGames + L"F4SE\\f4se.log";
    std::string text;
    if (g_myGames.empty() || !readShared(f4seLog, text)) {
        recorderWarning("f4se_log", "f4se.log could not be read: " + pathText(f4seLog));
        return;
    }
    std::string found = "[";
    size_t n = 0;
    for (const auto& l : lines(text)) {
        if (l.rfind("plugin ", 0) != 0 || l.rfind("plugin directory", 0) == 0)
            continue;
        if (n >= kMaxF4seLines)
            break;
        if (n++)
            found += ",";
        found += jsonString(l);
    }
    found += "]";
    Event("f4se_log").str("path", pathText(f4seLog)).raw("plugin_lines", found).write();
}

// ---- F4SE messages -------------------------------------------------------------------------

std::string fileNameOf(const char* data, std::uint32_t len)
{
    if (!data)
        return {};
    std::string p(data, strnlen(data, len ? len : 1024));
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? p : p.substr(slash + 1); // the save's name, not the user's path
}

void onMessage(f4se_abi::F4SEMessagingInterface::Message* msg)
{
    try {
        if (!msg)
            return;
        using M = f4se_abi::F4SEMessagingInterface;
        const char* name = nullptr;
        switch (msg->type) {
        case M::kPostLoad: name = "post_load"; break;
        case M::kPostPostLoad: name = "post_post_load"; break;
        case M::kPreLoadGame: name = "pre_load_game"; break;
        case M::kPostLoadGame: name = "post_load_game"; break;
        case M::kPreSaveGame: name = "pre_save_game"; break;
        case M::kPostSaveGame: name = "post_save_game"; break;
        case M::kDeleteGame: name = "delete_game"; break;
        case M::kInputLoaded: name = "input_loaded"; break;
        case M::kNewGame: name = "new_game"; break;
        case M::kGameLoaded: name = "game_loaded"; break;
        case M::kGameDataReady: name = "game_data_ready"; break;
        default: break;
        }
        Event e("f4se_message");
        e.str("message", name ? name : "unknown").num("type", msg->type);
        if (msg->type == M::kPreLoadGame || msg->type == M::kDeleteGame)
            e.str("save", fileNameOf(static_cast<const char*>(msg->data), msg->dataLen));
        if (msg->type == M::kPostLoadGame)
            e.flag("success", msg->data != nullptr);
        if (msg->type == M::kGameDataReady)
            e.flag("ready", msg->data != nullptr);
        e.write();
        if (msg->type == M::kPostLoad && !g_loadRecorded) {
            g_loadRecorded = true;
            recordLoadOrder();
            recordModules();
        }
    } catch (const std::exception& ex) {
        recorderError("message", ex.what());
    } catch (...) {
        recorderError("message", "unknown exception");
    }
}

} // namespace

// ---- F4SE entry points ---------------------------------------------------------------------

extern "C" {

// Read by F4SE 0.7.x (Next Gen) without running the DLL: no game addresses, no game structures.
__declspec(dllexport) f4se_abi::F4SEPluginVersionData F4SEPlugin_Version = {
    f4se_abi::F4SEPluginVersionData::kVersion,
    kRecorderVersion,
    "MultiplexerDiagnosticRecorder",
    "Fallout 4 Multiplexer Renewed",
    f4se_abi::F4SEPluginVersionData::kAddressIndependenceSignatures,
    f4se_abi::F4SEPluginVersionData::kStructureIndependenceNoStructs,
    {0},
    0,
    0,
    0,
    {0},
};

// Called by F4SE 0.6.x (Old Gen) before loading.
__declspec(dllexport) bool F4SEPlugin_Query(const f4se_abi::F4SEInterface* f4se, f4se_abi::PluginInfo* info)
{
    try {
        if (info) {
            info->infoVersion = 1;
            info->name = "MultiplexerDiagnosticRecorder";
            info->version = kRecorderVersion;
        }
        return f4se && !f4se->isEditor;
    } catch (...) {
        return false;
    }
}

__declspec(dllexport) bool F4SEPlugin_Load(const f4se_abi::F4SEInterface* f4se)
{
    try {
        if (!f4se || f4se->isEditor)
            return false;
        g_startTick = GetTickCount64();
        g_f4seVersion = f4se->f4seVersion;
        g_runtimeVersion = f4se->runtimeVersion;
        g_userProfile = knownFolder(FOLDERID_Profile);
        if (!g_userProfile.empty())
            g_userProfile.pop_back(); // "C:\Users\Name"
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        const std::wstring exePath(exe, n);
        g_gameDir = exePath.substr(0, exePath.find_last_of(L"\\/") + 1);
        // The My Games folder: F4SE 0.7.1+ says which; earlier versions always use "Fallout4".
        std::wstring saveFolder = L"Fallout4";
        if (g_f4seVersion >= f4se_abi::packVersion(0, 7, 1) && f4se->GetSaveFolderName) {
            if (const char* folder = f4se->GetSaveFolderName(); folder && *folder)
                saveFolder.assign(folder, folder + std::strlen(folder));
        }
        const std::wstring documents = knownFolder(FOLDERID_Documents);
        if (!documents.empty())
            g_myGames = documents + L"My Games\\" + saveFolder + L"\\";
        openSession();

        Event("session_start")
            .str("recorder_version", kRecorderVersionText)
            .str("runtime_version", versionText(g_runtimeVersion))
            .str("f4se_version", versionText(g_f4seVersion))
            .num("f4se_release_index", f4se->GetReleaseIndex ? f4se->GetReleaseIndex() : 0)
            .str("game_folder", pathText(g_gameDir))
            .str("game_exe_version", fileVersion(exePath))
            .num("process_id", GetCurrentProcessId())
            .write();

        g_handle = f4se->GetPluginHandle ? f4se->GetPluginHandle() : f4se_abi::kPluginHandleInvalid;
        auto* messaging = f4se->QueryInterface
                              ? static_cast<f4se_abi::F4SEMessagingInterface*>(f4se->QueryInterface(f4se_abi::kInterfaceMessaging))
                              : nullptr;
        if (!messaging || !messaging->RegisterListener) {
            recorderError("load", "the F4SE messaging interface is not available: game events will not be recorded");
            return true; // the session start is still useful
        }
        if (!messaging->RegisterListener(g_handle, "F4SE", onMessage))
            recorderError("load", "registering for F4SE messages failed: game events will not be recorded");
        return true;
    } catch (const std::exception& ex) {
        recorderError("load", ex.what());
        return true;
    } catch (...) {
        recorderError("load", "unknown exception");
        return true;
    }
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved)
{
    // A normal process exit (ExitProcess) passes a non-null `reserved`: the session ended without a
    // crash. A crash, or the process being terminated, never gets here, so the log has no end.
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr && g_file != INVALID_HANDLE_VALUE) {
        char line[192];
        std::snprintf(line, sizeof(line), "{\"schema\":%d,\"seq\":%llu,\"ms\":%llu,\"event\":\"session_end\",\"reason\":\"process_exit\"}\n",
                      kSchema, static_cast<unsigned long long>(g_seq),
                      static_cast<unsigned long long>(GetTickCount64() - g_startTick));
        DWORD written = 0;
        WriteFile(g_file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    return TRUE;
}
