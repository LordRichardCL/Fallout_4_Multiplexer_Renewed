#include "engine/SwitchBack.hpp"

#include "engine/BuildRecord.hpp"
#include "util/FileTransaction.hpp"
#include "util/Json.hpp"
#include "util/Text.hpp"

#include <cstdio>
#include <fstream>
#include <optional>

namespace fo4mx::switch_back {

namespace fs = std::filesystem;

namespace {

std::string hex(uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string currentHash(const fs::path& file)
{
    const auto h = buildrecord::hashFile(file);
    return h ? hex(*h) : std::string();
}

json::Value readAll(const fs::path& recordDirectory)
{
    std::ifstream in(recordDirectory / kRecordFile, std::ios::binary);
    if (!in)
        return json::Value(json::Array{});
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        auto v = json::Value::parse(text);
        if (v.isArray())
            return v;
    } catch (const std::exception&) {
    }
    return json::Value(json::Array{});
}

AppliedRecord fromJson(const json::Value& v)
{
    AppliedRecord r;
    r.when = v["when"].asString();
    r.build = v["build"].asString();
    r.switchedBack = v["switchedBack"].asBool();
    if (v["files"].isArray())
        for (const auto& f : v["files"].asArray()) {
            AppliedFile a;
            a.kind = f["kind"].asString() == "ghosted" ? AppliedFile::Kind::Ghosted : AppliedFile::Kind::Written;
            a.target = utf8ToPath(f["target"].asString());
            a.backup = utf8ToPath(f["backup"].asString());
            a.original = utf8ToPath(f["original"].asString());
            a.hashAfter = f["hashAfter"].asString();
            r.files.push_back(std::move(a));
        }
    return r;
}

} // namespace

void record(const FileTransaction& tx, const fs::path& recordDirectory, const std::string& build)
{
    json::Value entry(json::Object{});
    std::string stamp;
    json::Array files;
    for (const auto& op : tx.operations()) {
        if (!op.applied || op.skipped)
            continue;
        json::Value f(json::Object{});
        if (op.kind == FileTransaction::Kind::Write) {
            f.set("kind", "written");
            f.set("target", pathToUtf8(op.target));
            // An empty backup made because the file was missing records "there was no file".
            std::error_code ec;
            const bool hadFile = op.existedBefore && !op.backup.empty();
            f.set("backup", hadFile ? pathToUtf8(op.backup) : std::string());
            f.set("hashAfter", currentHash(op.target));
        } else if (op.kind == FileTransaction::Kind::Rename) {
            f.set("kind", "ghosted");
            f.set("target", pathToUtf8(op.target));
            f.set("original", pathToUtf8(op.source));
        } else {
            continue; // a copy into a downloads folder changes nothing in the game
        }
        files.push_back(std::move(f));
    }
    if (files.empty())
        return;
    for (const auto& op : tx.operations())
        if (!op.backup.empty()) {
            const std::string name = pathToUtf8(op.backup.filename());
            const auto at = name.find(".multiplexer-backup-");
            if (at != std::string::npos)
                stamp = name.substr(at + 20);
            break;
        }
    entry.set("when", stamp.empty() ? backupTimestamp() : stamp);
    entry.set("build", build);
    entry.set("switchedBack", false);
    entry.set("files", json::Value(std::move(files)));
    auto all = readAll(recordDirectory);
    all.push(std::move(entry));
    std::error_code ec;
    fs::create_directories(recordDirectory, ec);
    atomicWriteText(recordDirectory / kRecordFile, all.dump());
}

std::optional<AppliedRecord> latest(const fs::path& recordDirectory)
{
    const auto all = readAll(recordDirectory);
    if (all.asArray().empty())
        return std::nullopt;
    return fromJson(all.asArray().back());
}

Plan plan(FileTransaction& tx, const fs::path& recordDirectory)
{
    Plan p;
    const auto last = latest(recordDirectory);
    if (!last) {
        p.nothing = "No Apply to Game is recorded for this output folder";
        return p;
    }
    p.record = *last;
    if (last->switchedBack) {
        p.nothing = "The last Apply to Game (" + last->when + ") was already switched back";
        return p;
    }
    std::error_code ec;
    const std::string stamp = backupTimestamp();
    for (const auto& f : last->files) {
        if (f.kind == AppliedFile::Kind::Ghosted) {
            if (!fs::exists(f.target, ec)) {
                if (!fs::exists(f.original, ec))
                    p.problems.push_back(pathToUtf8(f.target) + " is gone, and so is " + pathToUtf8(f.original.filename()));
                continue; // already renamed back
            }
            if (fs::exists(f.original, ec)) {
                p.problems.push_back(pathToUtf8(f.original) + " exists again, so " + pathToUtf8(f.target.filename()) +
                                     " cannot be renamed back");
                continue;
            }
            tx.rename(f.target, f.original, "rename " + pathToUtf8(f.target.filename()) + " back to " +
                                                pathToUtf8(f.original.filename()));
            continue;
        }
        const bool present = fs::exists(f.target, ec);
        if (present && !f.hashAfter.empty() && currentHash(f.target) != f.hashAfter)
            p.changedSince.push_back(pathToUtf8(f.target));
        if (!f.backup.empty()) {
            if (!fs::exists(f.backup, ec)) {
                p.problems.push_back("the backup " + pathToUtf8(f.backup) + " is missing");
                continue;
            }
            tx.copy(f.backup, f.target, "restore " + pathToUtf8(f.target) + " as it was before Apply", true);
        } else if (present) {
            // It did not exist before Apply: set aside (never deleted).
            fs::path aside = f.target;
            aside += ".multiplexer-removed-" + stamp;
            tx.rename(f.target, aside, "set " + pathToUtf8(f.target) + " aside (it did not exist before Apply)");
        }
    }
    if (tx.empty() && p.problems.empty()) {
        p.nothing = "Every file is already as it was before the last Apply to Game";
        return p;
    }
    p.possible = p.problems.empty();
    return p;
}

void markSwitchedBack(const fs::path& recordDirectory)
{
    auto all = readAll(recordDirectory);
    if (all.asArray().empty())
        return;
    all.array().back().set("switchedBack", true);
    atomicWriteText(recordDirectory / kRecordFile, all.dump());
}

} // namespace fo4mx::switch_back
