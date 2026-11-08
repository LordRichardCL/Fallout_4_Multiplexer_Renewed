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

constexpr std::uint32_t kVersion = f4se_abi::packVersion(0, 2, 0);
constexpr const char* kVersionText = "0.2.0";
constexpr const char* kKey = "sResourceArchiveList2";

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

// Memory regions worth scanning: committed, readable, not guard pages; the executable and private memory
// (the game's heap), not mapped files.
template <class F>
void forEachRegion(bool writableOnly, F f)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto* at = static_cast<std::uint8_t*>(si.lpMinimumApplicationAddress);
    auto* end = static_cast<std::uint8_t*>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi{};
    while (at < end && VirtualQuery(at, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        auto* base = static_cast<std::uint8_t*>(mbi.BaseAddress);
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readableProt = prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READ ||
                                  prot == PAGE_EXECUTE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY;
        const bool writable = prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_WRITECOPY ||
                              prot == PAGE_EXECUTE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && readableProt && !(mbi.Protect & PAGE_GUARD) &&
            (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE) && (!writableOnly || writable))
            f(base, mbi.RegionSize);
        at = base + mbi.RegionSize;
    }
}

bool nameLike(const char* p)
{
    // A pointer next to the value that names the setting: "sResourceArchiveList2" (any case, with or
    // without ":Archive").
    static const std::string want = "sresourcearchivelist2";
    if (!p || !readable(p, want.size() + 1))
        return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(p[i])) != want[i])
            return false;
    return p[want.size()] == 0 || p[want.size()] == ':';
}

// Finds the game's SResourceArchiveList2 setting and points it at the full list. It looks for what is
// certain rather than for a layout: the text the game received (in its heap), the places that point at
// that text, and, next to such a place, a pointer to the setting's name.
void supplyList()
{
    if (g_full.empty())
        return;
    log("The game opens its first archive: " + std::to_string(g_reads) + " reads of " + kKey + " seen, the last returned " +
        std::to_string(g_received.size()) + " characters");
    if (g_received.size() < 32) {
        log("Not changed: the value the game read was not seen (another plugin may read the INI for the game)");
        return;
    }
    const DWORD started = GetTickCount();
    // 1. Where the received text is in memory.
    std::vector<const char*> copies;
    const std::string head = g_received.substr(0, 32);
    size_t scanned = 0;
    forEachRegion(false, [&](std::uint8_t* base, size_t size) {
        scanned += size;
        if (copies.size() >= 64 || size < head.size())
            return;
        const auto* p = base;
        const auto* last = base + size - head.size();
        while (p <= last) {
            p = static_cast<const std::uint8_t*>(std::memchr(p, head[0], static_cast<size_t>(last - p) + 1));
            if (!p)
                break;
            if (std::memcmp(p, head.data(), head.size()) == 0 &&
                static_cast<size_t>(base + size - p) > g_received.size() &&
                std::memcmp(p, g_received.data(), g_received.size()) == 0 && p[g_received.size()] == 0 &&
                reinterpret_cast<const char*>(p) != g_received.data())
                copies.push_back(reinterpret_cast<const char*>(p));
            ++p;
        }
    });
    log("Copies of the received value in memory: " + std::to_string(copies.size()) + " (" + std::to_string(scanned >> 20) +
        " MB scanned)");
    // 2. Places pointing at a copy, and 3. which of them have the setting's name beside them.
    struct Slot {
        char** slot;
        int nameOffset;
    };
    std::vector<Slot> pointers;
    forEachRegion(true, [&](std::uint8_t* base, size_t size) {
        for (size_t k = 0; k + 8 <= size; k += 8) {
            const auto v = *reinterpret_cast<const char* const*>(base + k);
            if (std::find(copies.begin(), copies.end(), v) == copies.end())
                continue;
            int nameAt = 0;
            for (int off : {8, -8, 16, -16, 24, -24}) {
                const auto at = static_cast<std::ptrdiff_t>(k) + off;
                if (at < 0 || static_cast<size_t>(at) + 8 > size)
                    continue;
                if (nameLike(*reinterpret_cast<const char* const*>(base + at))) {
                    nameAt = off;
                    break;
                }
            }
            pointers.push_back({reinterpret_cast<char**>(base + k), nameAt});
        }
    });
    std::vector<char**> chosen;
    for (const auto& p : pointers) {
        char where[64];
        std::snprintf(where, sizeof(where), "%p", static_cast<void*>(p.slot));
        log(std::string("  pointer at ") + where +
            (p.nameOffset ? ", setting name at " + std::to_string(p.nameOffset) + " bytes" : ", no setting name beside it"));
        if (p.nameOffset)
            chosen.push_back(p.slot);
    }
    log(std::to_string(pointers.size()) + " pointer(s) to the value, " + std::to_string(chosen.size()) +
        " with the setting's name beside them (" + std::to_string(GetTickCount() - started) + " ms)");
    if (chosen.empty() || chosen.size() > 4) {
        log("Not changed: the setting could not be identified with certainty");
        return;
    }
    // The supplied value stays allocated for the whole session (the game may keep using it).
    char* full = new char[g_full.size() + 1];
    std::memcpy(full, g_full.c_str(), g_full.size() + 1);
    for (auto* slot : chosen) {
        g_slots.emplace_back(slot, *slot);
        InterlockedExchangePointer(reinterpret_cast<PVOID*>(slot), full);
    }
    log("Supplied the full list to " + std::to_string(chosen.size()) + " setting(s): " + std::to_string(g_full.size()) +
        " characters (the game held " + std::to_string(g_received.size()) + "), " + std::to_string(g_wanted.size()) +
        " archives");
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
