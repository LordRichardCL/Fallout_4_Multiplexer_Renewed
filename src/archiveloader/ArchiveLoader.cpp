// MultiplexerArchiveLoader: PROOF OF CONCEPT. An F4SE plugin that gives Fallout 4 its archive list in
// full. The game reads SResourceArchiveList2 with GetPrivateProfileStringA into a 1,024-byte buffer and
// keeps only the first 1,023 characters (seen by the Diagnostic Recorder); the archives after that never
// load. This plugin waits until the game has read all its INI files (the moment it opens its first BA2
// archive), finds the game's own SResourceArchiveList2 setting, and points it at the full list, so the
// game's own code opens and registers every archive, in list order, as it does for a short list.
//
// The list comes from Data\F4SE\Plugins\MultiplexerArchives.txt (one archive per line, '#' comments),
// or, without that file, from SResourceArchiveList2 in Fallout4Custom.ini, read as plain text.
//
// How it finds the setting without game addresses: the setting is an object in the game's data whose
// name is the text "sResourceArchiveList2:Archive" (layout as in CommonLibF4: vtable, value, name). The
// plugin looks for that text in the executable, for objects pointing at it, and accepts only an object
// whose current value is exactly what the game received from the INI (seen through an observing wrapper
// of the game's GetPrivateProfileStringA import). Anything else, and it changes nothing. Once the game
// data is ready, the original value is put back. Everything it does is written to
// Documents\My Games\Fallout4\F4SE\MultiplexerArchiveLoader.log.

#include "../recorder/F4SEPluginAbi.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kVersion = f4se_abi::packVersion(0, 1, 0);
constexpr const char* kVersionText = "0.1.0";
constexpr const char* kKey = "sResourceArchiveList2";
constexpr const char* kSettingName = "sresourcearchivelist2:archive"; // compared in lower case

std::mutex g_lock;
HANDLE g_log = INVALID_HANDLE_VALUE;
std::wstring g_gameDir, g_myGames;
std::string g_full;         // the list to supply
std::string g_received;     // what the game received from the INI (last read of the key)
unsigned g_reads = 0;
std::atomic<bool> g_triggered{false};
std::vector<std::pair<char**, char*>> g_slots; // the setting objects supplied, with their values before
std::set<std::string> g_wanted; // the supplied archives (lower case)
std::set<std::string> g_opened; // those opened after the swap
f4se_abi::PluginHandle g_handle = f4se_abi::kPluginHandleInvalid;

using GetPrivateProfileStringA_t = DWORD(WINAPI*)(LPCSTR, LPCSTR, LPCSTR, LPSTR, DWORD, LPCSTR);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
GetPrivateProfileStringA_t g_prevRead = nullptr;
CreateFileA_t g_prevOpenA = nullptr;
CreateFileW_t g_prevOpenW = nullptr;

std::string lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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

void log(const std::string& text)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    const std::string line = stamp + text + "\r\n";
    DWORD written = 0;
    WriteFile(g_log, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    FlushFileBuffers(g_log);
}

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

bool readFile(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 16LL * 1024 * 1024) {
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
        std::string l = text.substr(at, nl - at);
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t'))
            l.pop_back();
        size_t b = 0;
        while (b < l.size() && (l[b] == ' ' || l[b] == '\t'))
            ++b;
        out.push_back(l.substr(b));
        at = nl + 1;
    }
    return out;
}

// The list to supply: the manifest, else the INI's own value.
void loadList()
{
    std::string text;
    const std::wstring manifest = g_gameDir + L"Data\\F4SE\\Plugins\\MultiplexerArchives.txt";
    if (readFile(manifest, text)) {
        for (const auto& l : lines(text))
            if (!l.empty() && l[0] != '#' && l[0] != ';')
                g_full += (g_full.empty() ? "" : ", ") + l;
        log("List from MultiplexerArchives.txt: " + std::to_string(g_full.size()) + " characters");
    } else if (!g_myGames.empty() && readFile(g_myGames + L"Fallout4Custom.ini", text)) {
        bool inArchive = false;
        for (const auto& l : lines(text)) {
            if (!l.empty() && l[0] == '[') {
                inArchive = lower(l).rfind("[archive]", 0) == 0;
                continue;
            }
            const auto eq = l.find('=');
            if (!inArchive || eq == std::string::npos)
                continue;
            std::string key = l.substr(0, eq);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
                key.pop_back();
            if (lower(key) == lower(kKey)) {
                g_full = l.substr(eq + 1);
                break; // the first one, as the game reads it
            }
        }
        log("List from Fallout4Custom.ini (no MultiplexerArchives.txt): " + std::to_string(g_full.size()) + " characters");
    } else {
        log("No MultiplexerArchives.txt and no Fallout4Custom.ini: nothing to supply");
    }
    size_t start = 0;
    while (start <= g_full.size()) {
        size_t comma = g_full.find(',', start);
        if (comma == std::string::npos)
            comma = g_full.size();
        std::string n = g_full.substr(start, comma - start);
        while (!n.empty() && n.front() == ' ')
            n.erase(n.begin());
        while (!n.empty() && n.back() == ' ')
            n.pop_back();
        if (!n.empty())
            g_wanted.insert(lower(n));
        start = comma + 1;
    }
}

bool readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT)
        return false;
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
    if (mbi.Protect & bad)
        return false;
    const auto end = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return static_cast<const std::uint8_t*>(p) + n <= end;
}

// Finds the game's SResourceArchiveList2 setting and points it at the full list.
void supplyList()
{
    if (g_full.empty())
        return;
    log("The game opens its first archive: " + std::to_string(g_reads) + " reads of " + kKey + " seen, the last returned " +
        std::to_string(g_received.size()) + " characters");
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    auto* sections = IMAGE_FIRST_SECTION(nt);
    const WORD count = nt->FileHeader.NumberOfSections;
    const std::string name = kSettingName;

    // 1. The setting's name in the executable ("sResourceArchiveList2:Archive", any letter case).
    std::vector<const std::uint8_t*> names;
    for (WORD i = 0; i < count; ++i) {
        const auto& s = sections[i];
        if (!(s.Characteristics & IMAGE_SCN_MEM_READ))
            continue;
        const std::uint8_t* p = base + s.VirtualAddress;
        const size_t size = std::min<size_t>(s.Misc.VirtualSize, imageSize - s.VirtualAddress);
        for (size_t k = 0; k + name.size() < size; ++k) {
            if (std::tolower(p[k]) != name[0])
                continue;
            size_t j = 1;
            while (j < name.size() && std::tolower(p[k + j]) == static_cast<unsigned char>(name[j]))
                ++j;
            if (j == name.size() && p[k + j] == 0 && (k == 0 || p[k - 1] == 0))
                names.push_back(p + k);
        }
    }
    log("Setting name found " + std::to_string(names.size()) + " time(s) in the executable");

    // 2. Objects pointing at it: name at +0x10, value at +0x08, vtable at +0x00 (CommonLibF4's Setting).
    struct Candidate {
        char** slot;
        const char* value;
        bool matches;
    };
    std::vector<Candidate> candidates;
    for (WORD i = 0; i < count; ++i) {
        const auto& s = sections[i];
        if (!(s.Characteristics & IMAGE_SCN_MEM_WRITE))
            continue; // settings live in writable data
        auto* p = base + s.VirtualAddress;
        const size_t size = std::min<size_t>(s.Misc.VirtualSize, imageSize - s.VirtualAddress);
        for (size_t k = 0x10; k + 8 <= size; k += 8) {
            const auto value = *reinterpret_cast<const std::uintptr_t*>(p + k);
            if (std::find(names.begin(), names.end(), reinterpret_cast<const std::uint8_t*>(value)) == names.end())
                continue;
            auto* obj = reinterpret_cast<std::uintptr_t*>(p + k - 0x10);
            const auto vtable = reinterpret_cast<std::uint8_t*>(obj[0]);
            if (vtable < base || vtable >= base + imageSize)
                continue;
            auto** slot = reinterpret_cast<char**>(p + k - 0x08);
            const char* current = *slot;
            bool matches = false;
            if (current && readable(current, 1)) {
                const size_t len = strnlen(current, 4096);
                if (readable(current, len + 1)) {
                    const std::string v(current, len);
                    matches = g_reads ? v == g_received : (!v.empty() && g_full.rfind(v, 0) == 0);
                }
            }
            candidates.push_back({slot, current, matches});
        }
    }
    std::vector<Candidate*> chosen;
    for (auto& c : candidates)
        if (c.matches)
            chosen.push_back(&c);
    log(std::to_string(candidates.size()) + " setting object(s) found, " + std::to_string(chosen.size()) +
        " holding exactly the value the game read");
    // Copies of the same setting (one per settings collection) all hold that value: all are supplied.
    if (chosen.empty() || chosen.size() > 4) {
        log("Not changed: the setting could not be identified with certainty");
        return;
    }
    const size_t had = std::strlen(chosen.front()->value);
    if (had >= g_full.size()) {
        log("Not changed: the game already holds the whole list (" + std::to_string(had) + " characters)");
        return;
    }
    // The supplied value stays allocated for the whole session (the game may keep using it).
    char* full = new char[g_full.size() + 1];
    std::memcpy(full, g_full.c_str(), g_full.size() + 1);
    for (auto* c : chosen) {
        g_slots.emplace_back(c->slot, *c->slot);
        InterlockedExchangePointer(reinterpret_cast<PVOID*>(c->slot), full);
    }
    log("Supplied the full list to " + std::to_string(chosen.size()) + " setting object(s): " +
        std::to_string(g_full.size()) + " characters (the game held " + std::to_string(had) + "), " +
        std::to_string(g_wanted.size()) + " archives");
}

std::string fileNameLower(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return lower(utf8(slash == std::wstring::npos ? path : path.substr(slash + 1)));
}

void beforeOpen(const std::wstring& path)
{
    if (path.size() < 4 || lower(utf8(path.substr(path.size() - 4))) != ".ba2")
        return;
    if (!g_triggered.exchange(true))
        supplyList();
}

void afterOpen(const std::wstring& path, HANDLE h)
{
    if (h == INVALID_HANDLE_VALUE || g_slots.empty())
        return;
    const std::string n = fileNameLower(path);
    if (g_wanted.count(n)) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_opened.insert(n);
    }
}

DWORD WINAPI onRead(LPCSTR app, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file)
{
    const DWORD r = g_prevRead(app, key, def, out, size, file);
    const DWORD err = GetLastError();
    try {
        if (key && out && size && lower(key) == lower(kKey)) {
            g_received.assign(out, std::min<size_t>(r, size - 1));
            ++g_reads;
        }
    } catch (...) {
    }
    SetLastError(err);
    return r;
}

HANDLE WINAPI onOpenW(LPCWSTR name, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
{
    try {
        if (name)
            beforeOpen(name);
    } catch (...) {
    }
    const HANDLE h = g_prevOpenW(name, a, s, sa, d, f, t);
    const DWORD err = GetLastError();
    try {
        if (name)
            afterOpen(name, h);
    } catch (...) {
    }
    SetLastError(err);
    return h;
}

HANDLE WINAPI onOpenA(LPCSTR name, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
{
    std::wstring w;
    try {
        if (name) {
            const int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
            if (n > 1) {
                w.assign(static_cast<size_t>(n - 1), L'\0');
                MultiByteToWideChar(CP_ACP, 0, name, -1, w.data(), n);
                beforeOpen(w);
            }
        }
    } catch (...) {
    }
    const HANDLE h = g_prevOpenA(name, a, s, sa, d, f, t);
    const DWORD err = GetLastError();
    try {
        if (!w.empty())
            afterOpen(w, h);
    } catch (...) {
    }
    SetLastError(err);
    return h;
}

void** importSlot(const char* function)
{
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return nullptr;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "kernel32.dll") != 0)
            continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0)
                return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}

template <class F>
bool wrapImport(const char* function, F wrapper, F& previous)
{
    void** slot = importSlot(function);
    if (!slot)
        return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        return false;
    previous = reinterpret_cast<F>(*slot);
    *slot = reinterpret_cast<void*>(wrapper);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

void onMessage(f4se_abi::F4SEMessagingInterface::Message* msg)
{
    try {
        if (!msg || msg->type != f4se_abi::F4SEMessagingInterface::kGameDataReady || !msg->data)
            return;
        if (g_slots.empty()) {
            log("Game data ready: the list was not supplied");
            return;
        }
        // Every archive has been opened by now: give the setting its own value back.
        for (const auto& [slot, original] : g_slots)
            InterlockedExchangePointer(reinterpret_cast<PVOID*>(slot), original);
        std::string missing;
        size_t shown = 0;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            for (const auto& w : g_wanted)
                if (!g_opened.count(w) && shown++ < 10)
                    missing += "\r\n    not opened: " + w;
        }
        log("Game data ready: " + std::to_string(g_opened.size()) + " of " + std::to_string(g_wanted.size()) +
            " supplied archives were opened after the list was supplied (archives not in Data are never opened; "
            "the Diagnostic Recorder's report compares in full)" + missing);
        log("Original value restored");
    } catch (...) {
    }
}

} // namespace

extern "C" {

__declspec(dllexport) f4se_abi::F4SEPluginVersionData F4SEPlugin_Version = {
    f4se_abi::F4SEPluginVersionData::kVersion,
    kVersion,
    "MultiplexerArchiveLoader",
    "Fallout 4 Multiplexer Renewed",
    f4se_abi::F4SEPluginVersionData::kAddressIndependenceSignatures,
    f4se_abi::F4SEPluginVersionData::kStructureIndependenceNoStructs,
    {0},
    0,
    0,
    0,
    {0},
};

__declspec(dllexport) bool F4SEPlugin_Query(const f4se_abi::F4SEInterface* f4se, f4se_abi::PluginInfo* info)
{
    if (info) {
        info->infoVersion = 1;
        info->name = "MultiplexerArchiveLoader";
        info->version = kVersion;
    }
    return f4se && !f4se->isEditor;
}

__declspec(dllexport) bool F4SEPlugin_Load(const f4se_abi::F4SEInterface* f4se)
{
    try {
        if (!f4se || f4se->isEditor)
            return false;
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        const std::wstring exePath(exe, n);
        g_gameDir = exePath.substr(0, exePath.find_last_of(L"\\/") + 1);
        std::wstring saveFolder = L"Fallout4";
        if (f4se->f4seVersion >= f4se_abi::packVersion(0, 7, 1) && f4se->GetSaveFolderName)
            if (const char* folder = f4se->GetSaveFolderName(); folder && *folder)
                saveFolder.assign(folder, folder + std::strlen(folder));
        const std::wstring documents = knownFolder(FOLDERID_Documents);
        if (!documents.empty()) {
            g_myGames = documents + L"My Games\\" + saveFolder + L"\\";
            CreateDirectoryW((g_myGames + L"F4SE").c_str(), nullptr);
            g_log = CreateFileW((g_myGames + L"F4SE\\MultiplexerArchiveLoader.log").c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        log(std::string("MultiplexerArchiveLoader ") + kVersionText + " (proof of concept) loaded");
        loadList();
        const bool r = wrapImport("GetPrivateProfileStringA", &onRead, g_prevRead);
        const bool w = wrapImport("CreateFileW", &onOpenW, g_prevOpenW);
        const bool a = wrapImport("CreateFileA", &onOpenA, g_prevOpenA);
        log(std::string("Watching the game's INI reads: ") + (r ? "yes" : "NO") + ", archive opens: " +
            (w || a ? "yes" : "NO"));
        g_handle = f4se->GetPluginHandle ? f4se->GetPluginHandle() : f4se_abi::kPluginHandleInvalid;
        if (auto* m = f4se->QueryInterface ? static_cast<f4se_abi::F4SEMessagingInterface*>(
                                                 f4se->QueryInterface(f4se_abi::kInterfaceMessaging))
                                           : nullptr;
            m && m->RegisterListener)
            m->RegisterListener(g_handle, "F4SE", onMessage);
        return true;
    } catch (...) {
        return true;
    }
}

} // extern "C"
