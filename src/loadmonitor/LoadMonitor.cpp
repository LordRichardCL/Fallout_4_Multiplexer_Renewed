// Fallout 4 Multiplexer Load Monitor (MultiplexerLoadMonitor.dll): an F4SE plugin that only watches.
// It shows and logs which part of the start-up or a load the game is in, and which file it is working
// on, so a long load or a hang can be narrowed down. It changes nothing: no load order, record, archive,
// file, engine limit or save is touched, and every observed call is passed on unchanged.
//
// What it observes, and how (see docs/LOAD_MONITOR.md for the full table):
//   - F4SE's own messages (PostLoad, GameDataReady, InputLoaded, GameLoaded, NewGame, PreLoadGame,
//     PostLoadGame, ...), through F4SE's messaging interface.
//   - The F4SE Papyrus registration callback: F4SE calls it once the game's script virtual machine has
//     been created and is registering native functions.
//   - The F4SE Scaleform callback: F4SE calls it for every menu movie the game creates.
//   - The game's file opens (CreateFileW, CreateFileA), reads (ReadFile) and closes (CloseHandle), by
//     wrapping Fallout4.exe's own kernel32 imports: each wrapper calls the function it replaced with the
//     same arguments and returns its result and error code. No game address, structure or hook in game
//     code is used, so it does not depend on the game version.
//
// The log is MultiplexerLoadMonitor.log beside the DLL (Data\F4SE\Plugins). The overlay is a small
// click-through window in the corner of the game window, drawn by its own thread so it keeps counting
// when the game stops responding. It needs windowed or borderless mode: over exclusive full screen it
// is not shown (the log has everything).
//
// LAST OBSERVED OPERATION != CONFIRMED CAUSE OF HANG. The monitor sees file activity and F4SE events
// only; most of the game's work (building forms, cells, scripts) happens in memory where nothing here
// can see it.

#include "../recorder/F4SEPluginAbi.hpp"
#include "MonitorCore.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace fo4mx::loadmon;
using Msg = f4se_abi::F4SEMessagingInterface;

constexpr std::uint32_t kVersion = f4se_abi::packVersion(0, 1, 0);
constexpr const char* kVersionText = "0.1.0";
constexpr const char* kName = "MultiplexerLoadMonitor";

// Settings, from MultiplexerLoadMonitor.ini beside the DLL ([Monitor] section); all optional.
struct Config {
    int overlay = -1;          // -1 auto (not over exclusive full screen), 0 off, 1 on
    bool looseFiles = false;   // also track loose files under Data (many thousands: off by default)
    bool trackReads = true;    // observe reads from tracked files
    bool menus = true;         // register the Scaleform callback to see menu movies being created
    long long quietMs = 15000; // no observed activity for this long while loading: a log note
    long long summaryMs = 5000;
} g_cfg;

HMODULE g_module = nullptr;
HANDLE g_log = INVALID_HANDLE_VALUE;
std::mutex g_logLock;
LARGE_INTEGER g_freq{}, g_start{};
std::wstring g_dataDirW;
std::string g_dataDir; // lower case, trailing separator
std::wstring g_myGames;

State g_state;
FileTable g_files;
std::atomic<TrackedFile*> g_lastRead{nullptr};
std::atomic<long long> g_lastReadAt{-1};
std::atomic<long long> g_lastRefresh{0};
std::atomic<bool> g_loading{true};   // inside start-up or a load: quiet notes and read summaries are on
std::atomic<bool> g_dataReady{false};
std::atomic<bool> g_stop{false};
std::atomic<unsigned> g_menus{0};
std::atomic<unsigned> g_failedOpens{0};
std::atomic<unsigned> g_looseOpens{0};
std::atomic<bool> g_sawArchive{false}, g_sawPlugin{false};
f4se_abi::PluginHandle g_handle = f4se_abi::kPluginHandleInvalid;

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFile_t = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using CloseHandle_t = BOOL(WINAPI*)(HANDLE);
CreateFileW_t g_prevOpenW = nullptr;
CreateFileA_t g_prevOpenA = nullptr;
ReadFile_t g_prevRead = nullptr;
CloseHandle_t g_prevClose = nullptr;
struct Wrapped {
    void** slot;
    void* ours;
    void* previous;
};
std::vector<Wrapped> g_wrapped; // put back if the DLL is ever unloaded before the process ends

// Milliseconds since F4SEPlugin_Load, in integers: the game changes the floating-point settings of some
// of its threads.
long long now()
{
    if (!g_freq.QuadPart)
        return 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const long long ticks = t.QuadPart - g_start.QuadPart;
    return ticks / g_freq.QuadPart * 1000 + ticks % g_freq.QuadPart * 1000 / g_freq.QuadPart;
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

std::wstring wide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One line: wall clock, then time since load. Lines are not flushed one by one (that would slow the
// game's loading); Windows keeps what was written even if the game hangs or is ended.
void log(const std::string& text, bool flush = false)
{
    std::lock_guard<std::mutex> lock(g_logLock);
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    const long long ms = now();
    char stamp[48];
    std::snprintf(stamp, sizeof(stamp), "[%02u:%02u:%02u.%03u +%4lld.%03llds] ", t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds, ms / 1000, ms % 1000);
    const std::string line = stamp + text + "\r\n";
    DWORD written = 0;
    WriteFile(g_log, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    if (flush)
        FlushFileBuffers(g_log);
}

void milestone(const std::string& stageName, const std::string& what)
{
    const long long t = now();
    g_lastRead.store(nullptr);
    g_state.stage(stageName, t);
    g_state.current(what, t);
    g_state.completed(what, t);
    log("Stage: " + stageName + " | " + what, true);
}

std::wstring moduleDir()
{
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH * 2);
    std::wstring p(path, n);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

std::wstring knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
        out = p;
        if (!out.empty() && out.back() != L'\\')
            out += L'\\';
    }
    CoTaskMemFree(p);
    return out;
}

void readConfig()
{
    const std::wstring file = moduleDir() + L"MultiplexerLoadMonitor.ini";
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(L"Monitor", L"Overlay", L"auto", buf, 32, file.c_str());
    const std::string o = lowerAscii(utf8(buf));
    g_cfg.overlay = (o == "0" || o == "off") ? 0 : (o == "1" || o == "on") ? 1 : -1;
    g_cfg.looseFiles = GetPrivateProfileIntW(L"Monitor", L"LooseFiles", 0, file.c_str()) != 0;
    g_cfg.trackReads = GetPrivateProfileIntW(L"Monitor", L"TrackReads", 1, file.c_str()) != 0;
    g_cfg.menus = GetPrivateProfileIntW(L"Monitor", L"MenuMovies", 1, file.c_str()) != 0;
    g_cfg.quietMs = 1000LL * std::max(5, static_cast<int>(GetPrivateProfileIntW(L"Monitor", L"QuietSeconds", 15, file.c_str())));
    g_cfg.summaryMs = 1000LL * std::max(1, static_cast<int>(GetPrivateProfileIntW(L"Monitor", L"SummarySeconds", 5, file.c_str())));
}

// ---- observed file activity -------------------------------------------------------------------------

void afterOpen(const std::wstring& name, HANDLE h)
{
    if (h == INVALID_HANDLE_VALUE) {
        g_failedOpens.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::string path = utf8(name);
    const FileKind kind = classify(path, g_dataDir);
    if (kind == FileKind::Other)
        return;
    if (kind == FileKind::Loose) {
        g_looseOpens.fetch_add(1, std::memory_order_relaxed);
        if (!g_cfg.looseFiles)
            return;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size))
        size.QuadPart = 0;
    TrackedFile* f = g_files.opened(reinterpret_cast<std::uintptr_t>(h), path, kind,
                                    static_cast<std::uint64_t>(size.QuadPart));
    const bool first = f->opens.load(std::memory_order_relaxed) == 1;
    const long long t = now();
    g_lastRead.store(nullptr);
    g_state.current(std::string("Opened ") + kindName(kind) + " " + f->name, t);
    if (kind == FileKind::Plugin)
        g_state.plugin(f->name);
    else if (kind == FileKind::Archive)
        g_state.archive(f->name);

    // Stages from the first file of a kind, before the game data is ready.
    if (!g_dataReady.load()) {
        if (kind == FileKind::Archive && !g_sawArchive.exchange(true))
            milestone("Opening archives (BA2)", "First archive opened: " + f->name);
        else if (kind == FileKind::Plugin && !g_sawPlugin.exchange(true))
            milestone("Opening plugin files", "First plugin file opened: " + f->name);
    }
    if (kind == FileKind::Save)
        log("Save file opened: " + path + " (" + megabytes(static_cast<std::uint64_t>(size.QuadPart)) + ")", true);
    else if (first)
        log(std::string("Opened ") + kindName(kind) + " " + path + " (" +
            megabytes(static_cast<std::uint64_t>(size.QuadPart)) + ", thread " + std::to_string(GetCurrentThreadId()) +
            ")");
}

HANDLE WINAPI onOpenW(LPCWSTR name, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
{
    const HANDLE h = g_prevOpenW(name, a, s, sa, d, f, t);
    const DWORD err = GetLastError();
    try {
        if (name && !g_stop.load(std::memory_order_relaxed))
            afterOpen(name, h);
    } catch (...) {
    }
    SetLastError(err);
    return h;
}

HANDLE WINAPI onOpenA(LPCSTR name, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
{
    const HANDLE h = g_prevOpenA(name, a, s, sa, d, f, t);
    const DWORD err = GetLastError();
    try {
        if (name && !g_stop.load(std::memory_order_relaxed)) {
            const int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
            if (n > 1) {
                std::wstring w(static_cast<size_t>(n - 1), L'\0');
                MultiByteToWideChar(CP_ACP, 0, name, -1, w.data(), n);
                afterOpen(w, h);
            }
        }
    } catch (...) {
    }
    SetLastError(err);
    return h;
}

BOOL WINAPI onRead(HANDLE h, LPVOID buffer, DWORD wanted, LPDWORD got, LPOVERLAPPED ov)
{
    const BOOL r = g_prevRead(h, buffer, wanted, got, ov);
    const DWORD err = GetLastError();
    try {
        if (TrackedFile* f = g_files.find(reinterpret_cast<std::uintptr_t>(h))) {
            // Bytes actually read for a plain read; the amount asked for when the read is asynchronous.
            const std::uint64_t n = (!ov && got && r) ? *got : wanted;
            f->bytes.fetch_add(n, std::memory_order_relaxed);
            f->reads.fetch_add(1, std::memory_order_relaxed);
            const long long t = now();
            g_lastReadAt.store(t, std::memory_order_relaxed);
            if (g_lastRead.exchange(f) != f) {
                g_state.current(readingText(*f), t);
                g_lastRefresh.store(t, std::memory_order_relaxed);
                if (f->kind == FileKind::Plugin)
                    g_state.plugin(f->name);
                else if (f->kind == FileKind::Archive)
                    g_state.archive(f->name);
            } else if (t - g_lastRefresh.load(std::memory_order_relaxed) >= 500) {
                g_lastRefresh.store(t, std::memory_order_relaxed);
                g_state.refresh(readingText(*f), t);
            }
        }
    } catch (...) {
    }
    SetLastError(err);
    return r;
}

BOOL WINAPI onClose(HANDLE h)
{
    try {
        g_files.closed(reinterpret_cast<std::uintptr_t>(h));
    } catch (...) {
    }
    return g_prevClose(h); // its own error code stands
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
    g_wrapped.push_back({slot, reinterpret_cast<void*>(wrapper), reinterpret_cast<void*>(previous)});
    return true;
}

// ---- F4SE events ------------------------------------------------------------------------------------

std::string counts()
{
    const auto t = g_files.totals();
    const auto k = [](FileKind f) { return static_cast<size_t>(f); };
    std::string s = std::to_string(t.files[k(FileKind::Archive)]) + " archives and " +
                    std::to_string(t.files[k(FileKind::Plugin)]) + " plugin files opened";
    if (g_cfg.trackReads)
        s += "; reads seen: plugins " + std::to_string(t.reads[k(FileKind::Plugin)]) + " (" +
             megabytes(t.bytes[k(FileKind::Plugin)]) + "), archives " + std::to_string(t.reads[k(FileKind::Archive)]) +
             " (" + megabytes(t.bytes[k(FileKind::Archive)]) + ")";
    return s;
}

void onMessage(Msg::Message* msg)
{
    try {
        if (!msg)
            return;
        const std::string thread = " (thread " + std::to_string(GetCurrentThreadId()) + ")";
        switch (msg->type) {
        case Msg::kPostLoad:
            milestone("F4SE plugins loaded", "F4SE message PostLoad" + thread);
            break;
        case Msg::kPostPostLoad:
            g_state.completed("F4SE message PostPostLoad", now());
            log("F4SE message PostPostLoad" + thread);
            break;
        case Msg::kGameDataReady:
            if (msg->data) {
                g_dataReady.store(true);
                milestone("Game data ready", "F4SE message GameDataReady: the game has loaded its plugins" + thread);
                log("At game data ready: " + counts() + "; loose files opened: " + std::to_string(g_looseOpens.load()) +
                        "; opens that failed (file not found and the like): " + std::to_string(g_failedOpens.load()),
                    true);
            } else {
                milestone("Loading game data", "F4SE message GameDataReady (sent before the plugins load)" + thread);
            }
            break;
        case Msg::kInputLoaded:
            milestone("Main menu initialising", "F4SE message InputLoaded: input loaded, main menu next" + thread);
            break;
        case Msg::kGameLoaded:
            g_loading.store(false);
            milestone("Main menu reached", "F4SE message GameLoaded" + thread);
            log("At main menu: " + counts(), true);
            break;
        case Msg::kNewGame:
            g_loading.store(true);
            milestone("New game: the engine is loading the world",
                      "F4SE message NewGame (world loading is inside the engine: not observable in v0.1)" + thread);
            break;
        case Msg::kPreLoadGame: {
            g_loading.store(true);
            std::string save;
            if (msg->data && msg->dataLen)
                save.assign(static_cast<const char*>(msg->data),
                            strnlen(static_cast<const char*>(msg->data), msg->dataLen));
            milestone("Loading a save", "F4SE message PreLoadGame: " + fileName(save) + thread);
            break;
        }
        case Msg::kPostLoadGame:
            g_loading.store(false);
            milestone(msg->data ? "Save loaded" : "Save load failed",
                      std::string("F4SE message PostLoadGame: ") + (msg->data ? "loaded" : "the game reports failure") +
                          thread);
            break;
        case Msg::kPreSaveGame:
            log("F4SE message PreSaveGame" + thread);
            break;
        case Msg::kPostSaveGame:
            log("F4SE message PostSaveGame" + thread);
            break;
        default:
            break;
        }
    } catch (...) {
    }
}

bool onPapyrus(void*)
{
    try {
        milestone("Papyrus virtual machine created",
                  "F4SE Papyrus callback: native functions are being registered (thread " +
                      std::to_string(GetCurrentThreadId()) + ")");
    } catch (...) {
    }
    return true;
}

bool onMenu(void*, void*)
{
    try {
        const unsigned n = g_menus.fetch_add(1) + 1;
        const std::string what = "Menu movie #" + std::to_string(n) + " created (F4SE Scaleform callback)";
        g_state.completed(what, now());
        if (n == 1)
            log("First menu movie created: the game's interface is running", true);
        else if (n <= 50)
            log(what);
        else if (n == 51)
            log("More menu movies are created; the count is in the summary at exit");
    } catch (...) {
    }
    return true;
}

// ---- watchdog: quiet notes and read summaries -------------------------------------------------------

DWORD WINAPI watchdog(LPVOID)
{
    long long reportedFor = -1; // the activity time the current quiet note is about
    long long nextNote = 0;
    long long lastSummary = 0;
    while (!g_stop.load()) {
        Sleep(1000);
        try {
            const long long t = now();
            Snapshot s = g_state.snapshot();
            s.lastActivity = std::max(s.lastActivity, g_lastReadAt.load(std::memory_order_relaxed));
            if (s.lastActivity < 0)
                continue;
            if (reportedFor >= 0 && s.lastActivity > reportedFor) {
                log("Activity observed again after " + duration(s.lastActivity - reportedFor) + ": " + s.current, true);
                reportedFor = -1;
            }
            if (g_loading.load() && t - s.lastActivity >= g_cfg.quietMs) {
                if (reportedFor != s.lastActivity) {
                    reportedFor = s.lastActivity;
                    nextNote = s.lastActivity + g_cfg.quietMs;
                }
                if (t >= nextNote) {
                    log(quietLine(s, t), true);
                    // 15 s, then every 30 s, growing to every 10 minutes for a long silence.
                    const long long gap = std::min<long long>(600000, std::max<long long>(30000, (t - s.lastActivity) / 2));
                    nextNote = t + gap;
                }
            }
            if (g_cfg.trackReads && g_loading.load() && t - lastSummary >= g_cfg.summaryMs) {
                const std::string line = intervalLine(g_files.takeInterval(), (t - lastSummary + 500) / 1000);
                lastSummary = t;
                if (!line.empty())
                    log(line);
            }
        } catch (...) {
        }
    }
    return 0;
}

// ---- overlay ----------------------------------------------------------------------------------------

HWND g_overlay = nullptr;
HFONT g_font = nullptr;
std::vector<std::wstring> g_lines;

struct FindGame {
    DWORD pid;
    HWND best = nullptr;
    long long area = 0;
};

BOOL CALLBACK findGameWindow(HWND w, LPARAM p)
{
    auto* f = reinterpret_cast<FindGame*>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != f->pid || w == g_overlay || !IsWindowVisible(w) || IsIconic(w))
        return TRUE;
    RECT r;
    if (!GetClientRect(w, &r))
        return TRUE;
    const long long area = static_cast<long long>(r.right) * r.bottom;
    if (r.right >= 640 && r.bottom >= 360 && area > f->area) {
        f->area = area;
        f->best = w;
    }
    return TRUE;
}

// Whether Fallout4Prefs.ini asks for exclusive full screen, where a window on top of the game can make
// it leave full screen: the overlay stays off there unless Overlay=1.
bool exclusiveFullScreen()
{
    if (g_myGames.empty())
        return false;
    const std::wstring prefs = g_myGames + L"Fallout4Prefs.ini";
    const int full = GetPrivateProfileIntW(L"Display", L"bFull Screen", 0, prefs.c_str());
    const int borderless = GetPrivateProfileIntW(L"Display", L"bBorderless", 0, prefs.c_str());
    return full != 0 && borderless == 0;
}

LRESULT CALLBACK overlayProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        HBRUSH bg = CreateSolidBrush(RGB(16, 16, 16));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        HGDIOBJ old = SelectObject(dc, g_font);
        SetBkMode(dc, TRANSPARENT);
        int y = 6;
        for (size_t i = 0; i < g_lines.size(); ++i) {
            SetTextColor(dc, i == 0 ? RGB(255, 200, 80) : i + 1 == g_lines.size() ? RGB(170, 170, 170) : RGB(235, 235, 235));
            RECT line{8, y, r.right - 8, y + 18};
            DrawTextW(dc, g_lines[i].c_str(), static_cast<int>(g_lines[i].size()), &line,
                      DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            y += 18;
        }
        SelectObject(dc, old);
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    return DefWindowProcW(w, m, wp, lp);
}

DWORD WINAPI overlayThread(LPVOID)
{
    try {
        WNDCLASSW wc{};
        wc.lpfnWndProc = overlayProc;
        wc.hInstance = g_module;
        wc.lpszClassName = L"MultiplexerLoadMonitorOverlay";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc))
            return 0;
        g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                        WS_EX_NOACTIVATE,
                                    wc.lpszClassName, L"Multiplexer Load Monitor", WS_POPUP, 0, 0, 520, 122, nullptr,
                                    nullptr, g_module, nullptr);
        if (!g_overlay)
            return 0;
        SetLayeredWindowAttributes(g_overlay, 0, 215, LWA_ALPHA);
        SetTimer(g_overlay, 1, 250, nullptr);
        bool shown = false, announced = false;
        long long idleSince = -1;
        MSG msg;
        while (!g_stop.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message != WM_TIMER || msg.hwnd != g_overlay) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                continue;
            }
            FindGame f{GetCurrentProcessId()};
            EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&f));
            const HWND fg = GetForegroundWindow();
            const long long t = now();
            // Hidden 10 s after a load has finished (main menu, save loaded); shown while loading.
            if (g_loading.load())
                idleSince = -1;
            else if (idleSince < 0)
                idleSince = t;
            // Shown while the game is in front. When the game stops responding, Windows puts a "ghost" window
            // of its own in front: the overlay stays while the game window is hung.
            DWORD fgPid = 0;
            if (fg)
                GetWindowThreadProcessId(fg, &fgPid);
            const bool front = fgPid == GetCurrentProcessId() || (shown && f.best && IsHungAppWindow(f.best));
            const bool want = f.best && front && (idleSince < 0 || t - idleSince < 10000);
            if (want) {
                if (!announced) {
                    announced = true;
                    log("Game window found: the overlay is shown (rendering has started)", true);
                }
                Snapshot s = g_state.snapshot();
                s.lastActivity = std::max(s.lastActivity, g_lastReadAt.load(std::memory_order_relaxed));
                g_lines.clear();
                for (const auto& l : overlayLines(s, t))
                    g_lines.push_back(wide(l));
                POINT corner{0, 0};
                ClientToScreen(f.best, &corner);
                SetWindowPos(g_overlay, HWND_TOPMOST, corner.x + 12, corner.y + 12, 520,
                             static_cast<int>(12 + 18 * g_lines.size()), SWP_NOACTIVATE | SWP_SHOWWINDOW);
                InvalidateRect(g_overlay, nullptr, FALSE);
                shown = true;
            } else if (shown) {
                ShowWindow(g_overlay, SW_HIDE);
                shown = false;
            }
        }
    } catch (...) {
    }
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_stop.store(true);
        // Unloaded without the process ending: give the game its own imports back first.
        if (!reserved)
            for (const auto& w : g_wrapped) {
                DWORD old = 0;
                if (*w.slot == w.ours && VirtualProtect(w.slot, sizeof(void*), PAGE_READWRITE, &old)) {
                    *w.slot = w.previous;
                    VirtualProtect(w.slot, sizeof(void*), old, &old);
                }
            }
        // The other threads may have been ended holding a lock: write the last line without one.
        if (g_log != INVALID_HANDLE_VALUE) {
            const char line[] = "Monitor unloading: the game is closing normally\r\n";
            DWORD written = 0;
            WriteFile(g_log, line, sizeof(line) - 1, &written, nullptr);
            FlushFileBuffers(g_log);
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
        }
    }
    return TRUE;
}

extern "C" {

__declspec(dllexport) f4se_abi::F4SEPluginVersionData F4SEPlugin_Version = {
    f4se_abi::F4SEPluginVersionData::kVersion,
    kVersion,
    "MultiplexerLoadMonitor",
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
        info->name = "MultiplexerLoadMonitor";
        info->version = kVersion;
    }
    return f4se && !f4se->isEditor;
}

__declspec(dllexport) bool F4SEPlugin_Load(const f4se_abi::F4SEInterface* f4se)
{
    try {
        if (!f4se || f4se->isEditor)
            return false;
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_start);
        readConfig();

        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        const std::wstring exePath(exe, n);
        g_dataDirW = exePath.substr(0, exePath.find_last_of(L"\\/") + 1) + L"Data\\";
        g_dataDir = lowerAscii(utf8(g_dataDirW));
        std::wstring saveFolder = L"Fallout4";
        if (f4se->f4seVersion >= f4se_abi::packVersion(0, 7, 1) && f4se->GetSaveFolderName)
            if (const char* folder = f4se->GetSaveFolderName(); folder && *folder)
                saveFolder.assign(folder, folder + std::strlen(folder));
        if (const std::wstring docs = knownFolder(FOLDERID_Documents); !docs.empty())
            g_myGames = docs + L"My Games\\" + saveFolder + L"\\";

        g_log = CreateFileW((moduleDir() + L"MultiplexerLoadMonitor.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_log == INVALID_HANDLE_VALUE && !g_myGames.empty()) {
            CreateDirectoryW((g_myGames + L"F4SE").c_str(), nullptr);
            g_log = CreateFileW((g_myGames + L"F4SE\\MultiplexerLoadMonitor.log").c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        char v[160];
        std::snprintf(v, sizeof(v), "F4SE %u.%u.%u, Fallout 4 %u.%u.%u", f4se->f4seVersion >> 24,
                      (f4se->f4seVersion >> 16) & 0xFF, (f4se->f4seVersion >> 4) & 0xFFF, f4se->runtimeVersion >> 24,
                      (f4se->runtimeVersion >> 16) & 0xFF, (f4se->runtimeVersion >> 4) & 0xFFF);
        log(std::string("Fallout 4 Multiplexer Load Monitor ") + kVersionText + " loaded (" + v + ", thread " +
                std::to_string(GetCurrentThreadId()) + "). Observation only: nothing is changed.",
            true);
        log(std::string("Note: ") + kNotACause + ". Times after the clock are seconds since this line.");
        log(std::string("Settings: overlay ") + (g_cfg.overlay < 0 ? "auto" : g_cfg.overlay ? "on" : "off") +
            ", loose files " + (g_cfg.looseFiles ? "tracked" : "counted only") + ", reads " +
            (g_cfg.trackReads ? "observed" : "not observed") + ", menu movies " + (g_cfg.menus ? "counted" : "not counted") +
            ", quiet note after " + std::to_string(g_cfg.quietMs / 1000) + " s");
        g_state.stage("F4SE loading plugins", now());
        g_state.completed("F4SEPlugin_Load", now());

        const bool w = wrapImport("CreateFileW", &onOpenW, g_prevOpenW);
        const bool a = wrapImport("CreateFileA", &onOpenA, g_prevOpenA);
        bool r = false, c = false;
        if (g_cfg.trackReads) {
            // Reads are attributed by handle, so they need the closes too (Windows reuses handle values).
            c = wrapImport("CloseHandle", &onClose, g_prevClose);
            r = c && wrapImport("ReadFile", &onRead, g_prevRead);
        }
        log(std::string("Observing the game's imports: CreateFileW ") + (w ? "yes" : "NO") + ", CreateFileA " +
            (a ? "yes" : "NO") + ", ReadFile " + (r ? "yes" : "NO") + ", CloseHandle " + (c ? "yes" : "NO"));

        g_handle = f4se->GetPluginHandle ? f4se->GetPluginHandle() : f4se_abi::kPluginHandleInvalid;
        bool messages = false, papyrus = false, menus = false;
        if (f4se->QueryInterface) {
            if (auto* m = static_cast<Msg*>(f4se->QueryInterface(f4se_abi::kInterfaceMessaging)); m && m->RegisterListener)
                messages = m->RegisterListener(g_handle, "F4SE", onMessage);
            if (auto* p = static_cast<f4se_abi::F4SEPapyrusInterface*>(f4se->QueryInterface(f4se_abi::kInterfacePapyrus));
                p && p->Register)
                papyrus = p->Register(onPapyrus);
            if (g_cfg.menus)
                if (auto* s = static_cast<f4se_abi::F4SEScaleformInterface*>(
                        f4se->QueryInterface(f4se_abi::kInterfaceScaleform));
                    s && s->Register)
                    menus = s->Register(kName, onMenu);
        }
        log(std::string("F4SE interfaces: messages ") + (messages ? "yes" : "NO") + ", Papyrus callback " +
            (papyrus ? "yes" : "NO") + ", Scaleform callback " + (menus ? "yes" : g_cfg.menus ? "NO" : "off"));

        if (HANDLE t = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr))
            CloseHandle(t);
        const bool fullScreen = exclusiveFullScreen();
        if (g_cfg.overlay == 1 || (g_cfg.overlay < 0 && !fullScreen)) {
            if (HANDLE t = CreateThread(nullptr, 0, overlayThread, nullptr, 0, nullptr))
                CloseHandle(t);
            log("Overlay: waiting for the game window");
        } else {
            log(g_cfg.overlay == 0 ? "Overlay: off (Overlay=0)"
                                   : "Overlay: off, the game is set to exclusive full screen (Fallout4Prefs.ini). "
                                     "Use borderless windowed, or Overlay=1 to show it anyway");
        }
        log("Ready", true);
        return true;
    } catch (...) {
        return true;
    }
}

} // extern "C"
