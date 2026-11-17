// MultiplexerArchiveLoader: an F4SE plugin that gives Fallout 4 its archive list in
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
//
// The log also traces the startup, to show when things happen rather than guess: every line has the
// time since F4SEPlugin_Load; each archive the game opens is listed once (number, thread, and whether
// it was opened before the list was supplied, from the list, or otherwise: base game, DLC, Creation
// Club, plugins); the first plugin file the game opens and every F4SE message are noted; and a watchdog
// thread writes what was last opened when the game opens no file for 20 seconds before its data is
// ready, so a hang documents itself. Only the game's own file opens are seen (its import table).

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

constexpr std::uint32_t kVersion = f4se_abi::packVersion(1, 1, 0);
constexpr const char* kVersionText = "1.1.0";
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

// Startup trace (g_traceLock; never held while calling log()).
LARGE_INTEGER g_freq{}, g_start{};
std::mutex g_traceLock;
std::set<std::string> g_seenArchives;  // archives opened so far (lower case)
unsigned g_archives = 0;               // distinct archives opened
unsigned g_reopens = 0;                // further opens of an archive already opened
std::string g_lastArchive;             // the last archive opened for the first time
DWORD g_lastArchiveThread = 0;
std::set<std::string> g_seenPlugins;   // plugin files (.esm/.esp/.esl) opened so far (lower case)
std::string g_lastFile;                // the last file of any kind the game opened
double g_lastActivity = -1;            // ms since load of the last file open (-1: none yet)
std::atomic<bool> g_dataReady{false};
std::atomic<bool> g_supplied{false};   // the full list has been supplied  // GameDataReady (plugins loaded) seen: the watchdog stops
constexpr double kStallMs = 20000;     // no file opened for this long before data ready: write a note

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

// Milliseconds since F4SEPlugin_Load.
double sinceLoad()
{
    if (!g_freq.QuadPart)
        return 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g_start.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
}

// One line: wall-clock time, then seconds since F4SEPlugin_Load. Trace lines (one per archive) are not
// flushed to disk one by one, which would slow the game's loading; Windows keeps written data even if
// the game hangs or is ended, and the next flushed line writes them too.
void log(const std::string& text, bool flush = true)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[48];
    std::snprintf(stamp, sizeof(stamp), "[%02u:%02u:%02u.%03u +%8.3fs] ", t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds, sinceLoad() / 1000.0);
    const std::string line = stamp + text + "\r\n";
    DWORD written = 0;
    WriteFile(g_log, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    if (flush)
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
    const auto at = reinterpret_cast<std::uintptr_t>(p);
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return at < end && n <= end - at; // no overflow for addresses near the top
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

int nameBeside(const std::uint8_t* base, size_t size, size_t k)
{
    for (int off : {8, -8, 16, -16, 24, -24}) {
        const auto at = static_cast<std::ptrdiff_t>(k) + off;
        if (at < 0 || static_cast<size_t>(at) + 8 > size)
            continue;
        if (nameLike(*reinterpret_cast<const char* const*>(base + at)))
            return off;
    }
    return 0;
}

// The quick way: the setting lives in the executable's writable data (where it was found in Fallout 4
// 1.10.163) and points at the text the game received. Each pointer-sized value there is checked against
// the committed memory map before it is read.
std::vector<char**> findInExecutable()
{
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> regions; // [begin, end), readable
    forEachRegion(false, [&](std::uint8_t* base, size_t size) {
        regions.emplace_back(reinterpret_cast<std::uintptr_t>(base), reinterpret_cast<std::uintptr_t>(base) + size);
    });
    auto inside = [&](std::uintptr_t v, size_t n) {
        auto it = std::upper_bound(regions.begin(), regions.end(), v,
                                   [](std::uintptr_t x, const auto& r) { return x < r.first; });
        if (it == regions.begin())
            return false;
        --it;
        return v >= it->first && v < it->second && n <= it->second - v; // no overflow for values near the top
    };
    std::vector<char**> found;
    auto* image = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + reinterpret_cast<IMAGE_DOS_HEADER*>(image)->e_lfanew);
    auto* sections = IMAGE_FIRST_SECTION(nt);
    const size_t want = g_received.size() + 1;
    auto nameNear = [&](std::uintptr_t at) {
        for (int off : {8, -8, 16, -16, 24, -24}) {
            const auto p = at + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(off));
            if (inside(p, 8) && nameLike(*reinterpret_cast<const char* const*>(p)))
                return true;
        }
        return false;
    };
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& sec = sections[i];
        if (!(sec.Characteristics & IMAGE_SCN_MEM_WRITE))
            continue;
        const auto secBegin = reinterpret_cast<std::uintptr_t>(image + sec.VirtualAddress);
        const auto secEnd =
            secBegin + std::min<size_t>(sec.Misc.VirtualSize, nt->OptionalHeader.SizeOfImage - sec.VirtualAddress);
        // A section spans several memory regions (protections differ within it): each readable part is scanned.
        for (const auto& [rb, re] : regions) {
            const auto from = std::max(rb, secBegin), to = std::min(re, secEnd);
            for (auto at = (from + 7) & ~std::uintptr_t(7); at + 8 <= to; at += 8) {
                const auto v = *reinterpret_cast<const std::uintptr_t*>(at);
                if (v < 0x10000 || !inside(v, want))
                    continue;
                const auto* text = reinterpret_cast<const char*>(v);
                if (text == g_received.data() || std::memcmp(text, g_received.data(), g_received.size()) != 0 ||
                    text[g_received.size()] != 0)
                    continue;
                if (nameNear(at))
                    found.push_back(reinterpret_cast<char**>(at));
            }
        }
    }
    if (found.size() > 4)
        found.clear();
    return found;
}

// Points the settings at the full list; the originals come back at game data ready.
void supplyTo(const std::vector<char**>& slots)
{
    if (slots.empty() || slots.size() > 4) {
        log("Not changed: the setting could not be identified with certainty");
        return;
    }
    // The supplied value stays allocated for the whole session (the game may keep using it).
    char* full = new char[g_full.size() + 1];
    std::memcpy(full, g_full.c_str(), g_full.size() + 1);
    for (auto* slot : slots) {
        g_slots.emplace_back(slot, *slot);
        InterlockedExchangePointer(reinterpret_cast<PVOID*>(slot), full);
    }
    g_supplied.store(true);
    log("Supplied the full list to " + std::to_string(slots.size()) + " setting(s): " + std::to_string(g_full.size()) +
        " characters (the game held " + std::to_string(g_received.size()) + "), " + std::to_string(g_wanted.size()) +
        " archives");
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
    // The list to supply must continue what the game read: a manifest from another build, or an INI
    // changed since, and nothing is changed.
    if (g_full.compare(0, g_received.size(), g_received) != 0) {
        log("Not changed: what the game read is not the start of the list to supply (the list is from another "
            "build, or the INI was changed after it was written)");
        return;
    }
    if (g_received.size() >= g_full.size()) {
        log("Not changed: the game read the whole list (" + std::to_string(g_received.size()) + " characters)");
        return;
    }
    const DWORD started = GetTickCount();
    std::vector<char**> chosen = findInExecutable();
    if (!chosen.empty()) {
        log("Found in the game's own data: " + std::to_string(chosen.size()) + " setting(s) (" +
            std::to_string(GetTickCount() - started) + " ms)");
        supplyTo(chosen);
        return;
    }
    log("Not found in the game's own data; searching all of its memory");
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
            pointers.push_back({reinterpret_cast<char**>(base + k), nameBeside(base, size, k)});
        }
    });
    chosen.clear();
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
    supplyTo(chosen);
}

std::string fileNameLower(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return lower(utf8(slash == std::wstring::npos ? path : path.substr(slash + 1)));
}

std::string extensionLower(const std::wstring& path)
{
    return path.size() < 4 ? std::string() : lower(utf8(path.substr(path.size() - 4)));
}

void beforeOpen(const std::wstring& path)
{
    if (extensionLower(path) != ".ba2")
        return;
    if (!g_triggered.exchange(true)) {
        log("Archive registration starts: the game opens its first archive, " + fileNameLower(path) + " (thread " +
            std::to_string(GetCurrentThreadId()) + ")");
        supplyList();
    }
}

// The startup trace: every archive once, the first plugin file, and the time of every open for the
// watchdog. Written after the open, so a line means the game has the file.
void traceOpen(const std::wstring& path, HANDLE h)
{
    if (g_dataReady.load())
        return;
    const std::string ext = extensionLower(path);
    const std::string name = fileNameLower(path);
    const bool archive = ext == ".ba2";
    const bool plugin = ext == ".esm" || ext == ".esp" || ext == ".esl";
    const DWORD thread = GetCurrentThreadId();
    std::string line;
    bool flush = false;
    {
        std::lock_guard<std::mutex> lock(g_traceLock);
        g_lastActivity = sinceLoad();
        g_lastFile = name;
        if (archive) {
            if (!g_seenArchives.insert(name).second) {
                ++g_reopens;
                return;
            }
            ++g_archives;
            g_lastArchive = name;
            g_lastArchiveThread = thread;
            // "list": named in MultiplexerArchives.txt (opened by the game's own list loading); "not in the
            // list": the game's own lists, DLC, Creation Club and plugins' archives.
            const char* phase = !g_wanted.count(name) ? "not in the list"
                                : g_supplied.load()    ? "list"
                                                       : "list, NOT supplied";
            line = "BA2 #" + std::to_string(g_archives) + "  " + name + "  thread " + std::to_string(thread) + "  [" +
                   phase + "]" + (h == INVALID_HANDLE_VALUE ? "  (open failed)" : "");
        } else if (plugin && h != INVALID_HANDLE_VALUE && g_seenPlugins.insert(name).second) {
            const size_t n = g_seenPlugins.size();
            if (n == 1) {
                line = "First plugin file opened by the game: " + name + " (thread " + std::to_string(thread) + ", after " +
                       std::to_string(g_archives) + " archives; archive registration is over)";
                flush = true;
            } else if (n % 250 == 0) {
                line = std::to_string(n) + " plugin files opened, the last " + name;
            }
        }
    }
    if (!line.empty())
        log(line, flush);
}

void afterOpen(const std::wstring& path, HANDLE h)
{
    traceOpen(path, h);
    if (h == INVALID_HANDLE_VALUE || !g_supplied.load())
        return;
    const std::string n = fileNameLower(path);
    if (g_wanted.count(n)) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_opened.insert(n);
    }
}

// Writes what the game last opened when it opens nothing for kStallMs before its data is ready: at 20
// seconds, then every minute while it lasts. A long plugin can take that long to read, so a note alone
// is not a hang; one that keeps repeating after the last archive, with no plugin file opened, is.
DWORD WINAPI watchdog(LPVOID)
{
    double noted = -1; // the activity time already reported
    double next = kStallMs;
    while (!g_dataReady.load()) {
        Sleep(1000);
        std::string line;
        {
            std::lock_guard<std::mutex> lock(g_traceLock);
            if (g_lastActivity < 0)
                continue;
            if (g_lastActivity != noted) {
                noted = g_lastActivity;
                next = kStallMs;
            }
            const double quiet = sinceLoad() - g_lastActivity;
            if (quiet < next)
                continue;
            next += 60000;
            char head[96];
            std::snprintf(head, sizeof(head), "Watchdog: no file opened for %.0f s. ", quiet / 1000.0);
            line = std::string(head) + "Last archive: #" + std::to_string(g_archives) + " " +
                   (g_lastArchive.empty() ? std::string("(none)") : g_lastArchive) + " (thread " +
                   std::to_string(g_lastArchiveThread) + "); last file: " + g_lastFile + "; " +
                   std::to_string(g_reopens) + " reopens; " +
                   (g_seenPlugins.empty() ? "no plugin file opened yet: the game is still in archive registration"
                                          : std::to_string(g_seenPlugins.size()) +
                                                " plugin files opened: the game is reading plugins");
        }
        log(line);
    }
    return 0;
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

const char* messageName(std::uint32_t type)
{
    using M = f4se_abi::F4SEMessagingInterface;
    switch (type) {
    case M::kPostLoad: return "PostLoad";
    case M::kPostPostLoad: return "PostPostLoad";
    case M::kPreLoadGame: return "PreLoadGame";
    case M::kPostLoadGame: return "PostLoadGame";
    case M::kPreSaveGame: return "PreSaveGame";
    case M::kPostSaveGame: return "PostSaveGame";
    case M::kDeleteGame: return "DeleteGame";
    case M::kInputLoaded: return "InputLoaded";
    case M::kNewGame: return "NewGame";
    case M::kGameLoaded: return "GameLoaded";
    case M::kGameDataReady: return "GameDataReady";
    default: return "unknown";
    }
}

void onMessage(f4se_abi::F4SEMessagingInterface::Message* msg)
{
    try {
        if (!msg)
            return;
        // Every F4SE message up to the first save activity, with the trace's counts at that moment.
        if (msg->type <= f4se_abi::F4SEMessagingInterface::kGameDataReady &&
            msg->type != f4se_abi::F4SEMessagingInterface::kPreSaveGame &&
            msg->type != f4se_abi::F4SEMessagingInterface::kPostSaveGame) {
            unsigned archives = 0, plugins = 0;
            {
                std::lock_guard<std::mutex> lock(g_traceLock);
                archives = g_archives;
                plugins = static_cast<unsigned>(g_seenPlugins.size());
            }
            std::string what = std::string("F4SE message ") + messageName(msg->type) + " (thread " +
                               std::to_string(GetCurrentThreadId()) + "): " + std::to_string(archives) +
                               " archives and " + std::to_string(plugins) + " plugin files opened so far";
            if (msg->type == f4se_abi::F4SEMessagingInterface::kGameDataReady)
                what += msg->data ? " (plugins loaded)" : " (sent before the plugins load)";
            log(what);
        }
        if (msg->type != f4se_abi::F4SEMessagingInterface::kGameDataReady || !msg->data)
            return;
        g_dataReady.store(true);
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
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_start);
        log(std::string("MultiplexerArchiveLoader ") + kVersionText + " loaded (F4SEPlugin_Load, thread " +
            std::to_string(GetCurrentThreadId()) + "); times after the clock are seconds since this line");
        // Only on the game version it was tested on: anywhere else it changes nothing.
        if (f4se->runtimeVersion != f4se_abi::packVersion(1, 10, 163)) {
            char v[32];
            std::snprintf(v, sizeof(v), "%u.%u.%u", (f4se->runtimeVersion >> 24) & 0xFF,
                          (f4se->runtimeVersion >> 16) & 0xFF, (f4se->runtimeVersion >> 4) & 0xFFF);
            log(std::string("Not active: tested on Fallout 4 1.10.163 only, this is ") + v +
                ". The archive list is read as the game reads it (cut at 1,023 characters)");
            return true;
        }
        loadList();
        const bool r = wrapImport("GetPrivateProfileStringA", &onRead, g_prevRead);
        const bool w = wrapImport("CreateFileW", &onOpenW, g_prevOpenW);
        const bool a = wrapImport("CreateFileA", &onOpenA, g_prevOpenA);
        log(std::string("Watching the game's INI reads: ") + (r ? "yes" : "NO") + ", archive opens: " +
            (w || a ? "yes" : "NO") + " (Fallout4.exe imports GetPrivateProfileStringA, CreateFileW, CreateFileA)");
        if (HANDLE t = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr)) {
            CloseHandle(t);
            log("Watchdog started: it notes when the game opens no file for 20 s before its data is ready");
        }
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
