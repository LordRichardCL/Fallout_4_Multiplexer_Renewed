#pragma once

// The platform-independent part of the Fallout 4 Multiplexer Load Monitor: what kind of file a path
// is, the shared record of what was last observed, per-file read counters, and the text of the log
// and overlay lines. No Windows and no game code here, so the unit tests build it on every platform.
//
// Everything this records is something that was observed to happen (an F4SE message arrived, the game
// opened a file, the game read from a file, an F4SE callback ran). It never infers that a step has
// finished, and it never moves to a stage because time passed. The last observed operation is not a
// confirmed cause of a hang: when the game goes quiet, the work it is doing may be inside the engine,
// where no file is opened or read and nothing here can see it.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fo4mx::loadmon {

constexpr const char* kNotACause =
    "LAST OBSERVED OPERATION != CONFIRMED CAUSE OF HANG: the game may be working inside the engine, where "
    "this monitor sees nothing";

enum class FileKind { Plugin, Archive, Save, Loose, Other };

inline std::string lowerAscii(std::string_view s)
{
    std::string out(s);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

inline bool endsWith(std::string_view s, std::string_view tail)
{
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

// The file name without its folders.
inline std::string fileName(std::string_view path)
{
    const auto cut = path.find_last_of("\\/");
    return std::string(cut == std::string_view::npos ? path : path.substr(cut + 1));
}

// Whether a lower-case path is inside the game's Data folder. dataDir is the lower-case full path of
// Data with a trailing separator; relative paths are taken from the game folder, which is how the game
// names most of its files ("Data\Textures\...").
inline bool underData(std::string_view lowerPath, std::string_view lowerDataDir)
{
    std::string p(lowerPath);
    std::replace(p.begin(), p.end(), '/', '\\');
    if (p.rfind(".\\", 0) == 0)
        p.erase(0, 2);
    if (p.rfind("data\\", 0) == 0)
        return true;
    std::string d(lowerDataDir);
    std::replace(d.begin(), d.end(), '/', '\\');
    return !d.empty() && p.rfind(d, 0) == 0;
}

inline FileKind classify(std::string_view path, std::string_view lowerDataDir)
{
    const std::string p = lowerAscii(path);
    if (endsWith(p, ".esm") || endsWith(p, ".esp") || endsWith(p, ".esl"))
        return FileKind::Plugin;
    if (endsWith(p, ".ba2"))
        return FileKind::Archive;
    if (endsWith(p, ".ess"))
        return FileKind::Save;
    return underData(p, lowerDataDir) ? FileKind::Loose : FileKind::Other;
}

inline const char* kindName(FileKind k)
{
    switch (k) {
    case FileKind::Plugin: return "plugin";
    case FileKind::Archive: return "archive";
    case FileKind::Save: return "save";
    case FileKind::Loose: return "loose file";
    default: return "file";
    }
}

// "12.3 s", "4 min 05 s", "1 h 02 min".
inline std::string duration(long long ms)
{
    if (ms < 0)
        ms = 0;
    char buf[48];
    if (ms < 60000)
        std::snprintf(buf, sizeof(buf), "%lld.%lld s", ms / 1000, ms % 1000 / 100);
    else if (ms < 3600000)
        std::snprintf(buf, sizeof(buf), "%lld min %02lld s", ms / 60000, ms % 60000 / 1000);
    else
        std::snprintf(buf, sizeof(buf), "%lld h %02lld min", ms / 3600000, ms % 3600000 / 60000);
    return buf;
}

inline std::string megabytes(std::uint64_t bytes)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

// The shared record of what was last observed. Every member is written under one lock; readers take a
// copy. Times are milliseconds since the monitor loaded.
struct Snapshot {
    std::string stage = "Starting";
    long long stageSince = 0;
    std::string current;            // the item being worked on, as last observed
    long long currentSince = 0;
    std::string previous;           // the item before it
    std::string lastCompleted;      // the last event known to have happened in full (a message, a callback)
    std::string lastPlugin;         // the last plugin file opened or read
    std::string lastArchive;        // the last archive opened or read
    long long lastActivity = -1;    // the last observation of any kind (-1: none yet)
};

class State {
public:
    // Stages change only on an observed event, never on a timer.
    void stage(std::string name, long long now)
    {
        std::lock_guard<std::mutex> lock(m_);
        if (s_.stage == name)
            return touchLocked(now);
        s_.stage = std::move(name);
        s_.stageSince = now;
        touchLocked(now);
    }

    void current(std::string item, long long now)
    {
        std::lock_guard<std::mutex> lock(m_);
        if (s_.current != item) {
            if (!s_.current.empty())
                s_.previous = std::move(s_.current);
            s_.current = std::move(item);
            s_.currentSince = now;
        }
        touchLocked(now);
    }

    void completed(std::string what, long long now)
    {
        std::lock_guard<std::mutex> lock(m_);
        s_.lastCompleted = std::move(what);
        touchLocked(now);
    }

    void plugin(std::string name)
    {
        std::lock_guard<std::mutex> lock(m_);
        s_.lastPlugin = std::move(name);
    }

    void archive(std::string name)
    {
        std::lock_guard<std::mutex> lock(m_);
        s_.lastArchive = std::move(name);
    }

    // Replaces the current item's text (its counters changed) without restarting its time.
    void refresh(std::string item, long long now)
    {
        std::lock_guard<std::mutex> lock(m_);
        s_.current = std::move(item);
        touchLocked(now);
    }

    void touch(long long now)
    {
        std::lock_guard<std::mutex> lock(m_);
        touchLocked(now);
    }

    Snapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_);
        return s_;
    }

private:
    void touchLocked(long long now)
    {
        if (now > s_.lastActivity)
            s_.lastActivity = now;
    }

    mutable std::mutex m_;
    Snapshot s_;
};

// One tracked file: every open of the same path shares it, so the counters are per file for the session.
struct TrackedFile {
    std::string name;  // file name only
    FileKind kind = FileKind::Other;
    std::atomic<std::uint64_t> size{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint32_t> opens{0};
    std::uint64_t summarised = 0; // bytes at the last interval summary (summary thread only)
};

// Which open handle belongs to which tracked file, for observing reads. Look-ups take a shared lock and
// touch only atomics, so the game's reading threads do not wait for each other.
class FileTable {
public:
    TrackedFile* opened(std::uintptr_t handle, const std::string& path, FileKind kind, std::uint64_t size)
    {
        std::unique_lock<std::shared_mutex> lock(m_);
        const std::string key = lowerAscii(path);
        auto it = byPath_.find(key);
        TrackedFile* f = nullptr;
        if (it == byPath_.end()) {
            files_.push_back(std::make_unique<TrackedFile>());
            f = files_.back().get();
            f->name = fileName(path);
            f->kind = kind;
            byPath_.emplace(key, f);
        } else {
            f = it->second;
        }
        if (size)
            f->size.store(size, std::memory_order_relaxed);
        f->opens.fetch_add(1, std::memory_order_relaxed);
        byHandle_[handle] = f;
        return f;
    }

    void closed(std::uintptr_t handle)
    {
        {
            std::shared_lock<std::shared_mutex> lock(m_);
            if (!byHandle_.count(handle))
                return;
        }
        std::unique_lock<std::shared_mutex> lock(m_);
        byHandle_.erase(handle);
    }

    TrackedFile* find(std::uintptr_t handle) const
    {
        std::shared_lock<std::shared_mutex> lock(m_);
        const auto it = byHandle_.find(handle);
        return it == byHandle_.end() ? nullptr : it->second;
    }

    // Files read from since the last call, most bytes first: (file, bytes in the interval).
    std::vector<std::pair<const TrackedFile*, std::uint64_t>> takeInterval()
    {
        std::vector<std::pair<const TrackedFile*, std::uint64_t>> out;
        std::shared_lock<std::shared_mutex> lock(m_);
        for (const auto& f : files_) {
            const std::uint64_t now = f->bytes.load(std::memory_order_relaxed);
            if (now > f->summarised)
                out.emplace_back(f.get(), now - f->summarised);
            f->summarised = now;
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        return out;
    }

    struct Totals {
        std::size_t files[5] = {};
        std::uint64_t reads[5] = {};
        std::uint64_t bytes[5] = {};
    };

    Totals totals() const
    {
        Totals t;
        std::shared_lock<std::shared_mutex> lock(m_);
        for (const auto& f : files_) {
            const auto k = static_cast<std::size_t>(f->kind);
            ++t.files[k];
            t.reads[k] += f->reads.load(std::memory_order_relaxed);
            t.bytes[k] += f->bytes.load(std::memory_order_relaxed);
        }
        return t;
    }

private:
    mutable std::shared_mutex m_;
    std::deque<std::unique_ptr<TrackedFile>> files_;
    std::unordered_map<std::string, TrackedFile*> byPath_;
    std::unordered_map<std::uintptr_t, TrackedFile*> byHandle_;
};

// "Reading Fallout4.esm (12.0 MB of 312.4 MB requested so far)". The amount is what the game asked
// for, which is not progress: a file can be read more than once, or not in order.
inline std::string readingText(const TrackedFile& f)
{
    std::string s = "Reading " + f.name + " (" + megabytes(f.bytes.load(std::memory_order_relaxed));
    if (const std::uint64_t size = f.size.load(std::memory_order_relaxed))
        s += " of " + megabytes(size);
    return s + " read so far)";
}

// The log line for the interval summary, or empty when nothing was read.
inline std::string intervalLine(const std::vector<std::pair<const TrackedFile*, std::uint64_t>>& reads,
                                long long seconds, std::size_t shown = 4)
{
    if (reads.empty())
        return {};
    std::string s = "Reads in the last " + std::to_string(seconds) + " s: ";
    for (std::size_t i = 0; i < reads.size() && i < shown; ++i)
        s += (i ? ", " : "") + reads[i].first->name + " " + megabytes(reads[i].second);
    if (reads.size() > shown)
        s += " (+" + std::to_string(reads.size() - shown) + " more files)";
    return s;
}

// The watchdog's note when nothing has been observed for a while.
inline std::string quietLine(const Snapshot& s, long long now)
{
    std::string line = "No observed activity for " + duration(now - s.lastActivity) + ". Stage: " + s.stage +
                       " (for " + duration(now - s.stageSince) + "). Last observed: " +
                       (s.current.empty() ? std::string("(nothing yet)") : s.current) + ". Previous: " +
                       (s.previous.empty() ? std::string("(none)") : s.previous) + ". Last plugin: " +
                       (s.lastPlugin.empty() ? std::string("(none)") : s.lastPlugin) + ". Last archive: " +
                       (s.lastArchive.empty() ? std::string("(none)") : s.lastArchive) + ". ";
    return line + kNotACause;
}

// The overlay's text, one entry per line.
inline std::vector<std::string> overlayLines(const Snapshot& s, long long now)
{
    std::vector<std::string> out;
    out.push_back("Multiplexer Load Monitor");
    out.push_back("Stage: " + s.stage);
    out.push_back("Current: " + (s.current.empty() ? std::string("(nothing observed yet)") : s.current));
    out.push_back("Previous: " + (s.previous.empty() ? std::string("(none)") : s.previous));
    std::string times = "On current: " + duration(s.current.empty() ? 0 : now - s.currentSince);
    if (s.lastActivity >= 0 && now - s.lastActivity >= 5000)
        times += "   No new activity: " + duration(now - s.lastActivity);
    out.push_back(times);
    out.push_back("Last observed is not a confirmed cause");
    return out;
}

} // namespace fo4mx::loadmon
