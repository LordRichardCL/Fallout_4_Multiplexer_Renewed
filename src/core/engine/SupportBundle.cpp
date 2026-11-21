#include "engine/SupportBundle.hpp"

#include "util/Text.hpp"
#include "util/Zip.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>

namespace fo4mx {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kMaxTextBytes = 20ull << 20; // logs can grow: the last 20 MB of each

bool isText(const fs::path& p)
{
    static const std::set<std::string> ext = {".txt", ".json", ".jsonl", ".tsv", ".html", ".htm", ".log",
                                              ".ini", ".csv", ".md"};
    return ext.count(toLower(pathToUtf8(p.extension()))) != 0;
}

std::string readTail(const fs::path& p, uint64_t max)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    in.seekg(0, std::ios::end);
    const auto size = static_cast<uint64_t>(in.tellg());
    const uint64_t start = size > max ? size - max : 0;
    in.seekg(static_cast<std::streamoff>(start));
    std::string out(static_cast<size_t>(size - start), '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    if (start)
        out = "[... the first " + std::to_string(start) + " bytes are left out ...]\n" + out;
    return out;
}

} // namespace

std::string redactText(std::string text, const std::vector<std::string>& secrets)
{
    std::vector<std::string> all;
    for (const auto& s : secrets) {
        if (s.size() < 3)
            continue; // too short to replace safely
        all.push_back(s);
        std::string fwd = s;
        std::replace(fwd.begin(), fwd.end(), '\\', '/');
        all.push_back(fwd);
        std::string back = s;
        std::replace(back.begin(), back.end(), '/', '\\');
        all.push_back(back);
    }
    // Longest first, so a home folder goes before the user name inside it.
    std::sort(all.begin(), all.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const auto& s : all) {
        // Case-insensitive (Windows paths come in any case).
        const std::string lowerS = toLower(s);
        std::string lower = toLower(text);
        size_t at = 0;
        const bool component = s.size() > 2 && (s.front() == '\\' || s.front() == '/') && s.back() == s.front();
        const std::string with = component ? std::string(1, s.front()) + "<user>" + std::string(1, s.back()) : "<user>";
        while ((at = lower.find(lowerS, at)) != std::string::npos) {
            text.replace(at, s.size(), with);
            lower.replace(at, s.size(), with);
            at += with.size();
        }
    }
    return text;
}

std::vector<std::string> personalDetails()
{
    std::vector<std::string> out;
    for (const char* var : {"USERPROFILE", "HOME"})
        if (const char* v = std::getenv(var); v && *v)
            out.push_back(v);
    // The user name only as a folder in a path (it could also be part of a mod's name).
    for (const char* var : {"USERNAME", "USER"})
        if (const char* v = std::getenv(var); v && *v && std::string(v) != "root")
            out.push_back("\\" + std::string(v) + "\\");
    return out;
}

SupportBundleResult writeSupportBundle(const SupportBundleInputs& in, const fs::path& zip)
{
    SupportBundleResult r;
    r.zip = zip;
    std::vector<std::string> secrets = personalDetails();
    secrets.insert(secrets.end(), in.redact.begin(), in.redact.end());

    std::error_code ec;
    const fs::path stage = zip.parent_path() / (pathToUtf8(zip.stem()) + ".staging");
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    // The staging folder goes whatever happens (it sits beside the zip, on the Desktop by default).
    struct Cleanup {
        fs::path dir;
        ~Cleanup()
        {
            std::error_code e;
            fs::remove_all(dir, e);
        }
    } cleanup{stage};

    auto add = [&](const fs::path& from, const std::string& to) {
        std::error_code e;
        if (from.empty() || !fs::is_regular_file(from, e)) {
            if (!from.empty())
                r.missing.push_back(to);
            return;
        }
        if (std::find(r.included.begin(), r.included.end(), to) != r.included.end())
            return; // the same report found in two places
        const fs::path dest = stage / utf8ToPath(to);
        fs::create_directories(dest.parent_path(), e);
        if (isText(from)) {
            std::ofstream out(dest, std::ios::binary);
            const std::string text = redactText(readTail(from, kMaxTextBytes), secrets);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
        } else {
            fs::copy_file(from, dest, fs::copy_options::overwrite_existing, e);
        }
        r.included.push_back(to);
    };
    auto addText = [&](const std::string& text, const std::string& to) {
        const fs::path dest = stage / utf8ToPath(to);
        std::error_code e;
        fs::create_directories(dest.parent_path(), e);
        std::ofstream out(dest, std::ios::binary);
        const std::string clean = redactText(text, secrets);
        out.write(clean.data(), static_cast<std::streamsize>(clean.size()));
        r.included.push_back(to);
    };

    // The export: its notes and the build's own records (never plugins, archives or other mod files).
    if (!in.packageRoot.empty()) {
        for (const char* name : {"Multiplexer_Instructions.txt", "Multiplexer_Run.txt", "Multiplexer_Build.json",
                                 "Multiplexer_DisablePlugins.txt", "Multiplexer_plugins.txt", "Multiplexer_Archives.ini"})
            if (auto p = findPathCaseInsensitive(in.packageRoot, name); !p.empty())
                add(p, std::string("export/") + name);
        for (const auto& dir : {in.packageRoot / "Data", in.packageRoot})
            for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
                const std::string name = pathToUtf8(it->path().filename());
                if (it->is_regular_file(ec) && istartsWith(name, "Multiplexer_") &&
                    (iendsWith(name, ".tsv") || iendsWith(name, ".txt") || iendsWith(name, ".json")) &&
                    std::find(r.included.begin(), r.included.end(), "export/" + name) == r.included.end())
                    add(it->path(), "export/" + name);
            }
        const auto reports = in.packageRoot / "Multiplexer Reports";
        for (auto it = fs::directory_iterator(reports, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec))
                add(it->path(), "reports/" + pathToUtf8(it->path().filename()));
    }
    if (!in.reportDirectory.empty() && in.reportDirectory != in.packageRoot / "Multiplexer Reports")
        for (auto it = fs::directory_iterator(in.reportDirectory, ec); !ec && it != fs::directory_iterator();
             it.increment(ec))
            if (it->is_regular_file(ec))
                add(it->path(), "reports/" + pathToUtf8(it->path().filename()));
    for (const auto& l : in.logs)
        add(l, "logs/" + pathToUtf8(l.filename()));
    for (const auto& f : in.loadOrder)
        add(f, "load order/" + pathToUtf8(f.filename()));
    if (!in.crashLog.empty()) {
        add(in.crashLog, "crash/" + pathToUtf8(in.crashLog.filename()));
        if (!in.crashReading.empty())
            addText(in.crashReading, "crash/" + pathToUtf8(in.crashLog.filename()) + ".reading.txt");
    }
    if (!in.recorderSession.empty())
        add(in.recorderSession, "recorder/" + pathToUtf8(in.recorderSession.filename()));
    for (const auto& [from, to] : in.extraFiles)
        add(from, to);
    for (const auto& [to, text] : in.readings)
        if (!text.empty())
            addText(text, to);
    {
        std::string s = "# Fallout 4 Multiplexer Renewed settings\n";
        for (const auto& [k, v] : in.settings)
            s += k + " = " + v + "\n";
        addText(s, "settings.txt");
    }
    {
        std::string readme = "Fallout 4 Multiplexer Renewed support bundle\n\nContents:\n";
        for (const auto& f : r.included)
            readme += "  " + f + "\n";
        if (!r.missing.empty()) {
            readme += "\nNot found:\n";
            for (const auto& f : r.missing)
                readme += "  " + f + "\n";
        }
        readme += "\nPersonal details (home folder, user name) are replaced with <user> in every text file. No "
                  "plugins, archives or other mod files are included.\n";
        addText(readme, "README.txt");
    }
    writeZip(zip, stage, "");
    return r;
}

} // namespace fo4mx
