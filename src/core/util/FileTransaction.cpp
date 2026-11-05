#include "util/FileTransaction.hpp"

#include "util/Text.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace fo4mx {

namespace fs = std::filesystem;

namespace {

std::optional<std::string> readWhole(const fs::path& p)
{
    std::error_code ec;
    if (!fs::is_regular_file(p, ec))
        return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + pathToUtf8(p));
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

fs::path tempBeside(const fs::path& file)
{
    auto tmp = file;
    tmp += ".multiplexer-tmp";
    return tmp;
}

// Copies `from` over `to` through a temporary file beside `to`.
void atomicCopy(const fs::path& from, const fs::path& to)
{
    const auto tmp = tempBeside(to);
    std::error_code ec;
    fs::remove(tmp, ec);
    fs::copy_file(from, tmp);
    try {
        fs::rename(tmp, to);
    } catch (...) {
        fs::remove(tmp, ec);
        throw;
    }
}

void makeBackup(FileTransaction::Operation& op, const std::string& stamp, bool fileExists)
{
    op.backup = uniqueBackupPath(op.target, stamp);
    if (fileExists)
        fs::copy_file(op.target, op.backup); // never overwrites: the name was free a moment ago
    else
        atomicWriteText(op.backup, "");
    spdlog::info("Backed up {} to {}", pathToUtf8(op.target), pathToUtf8(op.backup));
}

const char* verb(FileTransaction::Kind k)
{
    switch (k) {
    case FileTransaction::Kind::Write: return "Write";
    case FileTransaction::Kind::Rename: return "Rename";
    case FileTransaction::Kind::Copy: return "Copy";
    }
    return "?";
}

} // namespace

std::string backupTimestamp()
{
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

fs::path uniqueBackupPath(const fs::path& file, const std::string& stamp)
{
    std::error_code ec;
    auto base = file;
    base += ".multiplexer-backup-" + stamp;
    if (!fs::exists(base, ec))
        return base;
    for (int n = 2; n < 100000; ++n) {
        auto candidate = base;
        candidate += "-" + std::to_string(n);
        if (!fs::exists(candidate, ec))
            return candidate;
    }
    throw std::runtime_error("no free backup name for " + pathToUtf8(file));
}

void atomicWriteText(const fs::path& file, std::string_view text)
{
    const auto tmp = tempBeside(file);
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write " + pathToUtf8(file));
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            throw std::runtime_error("cannot write " + pathToUtf8(file));
        }
    }
    try {
        fs::rename(tmp, file);
    } catch (...) {
        fs::remove(tmp, ec);
        throw;
    }
}

FileTransaction::FileTransaction(std::string name) : name_(std::move(name)), stamp_(backupTimestamp()) {}

void FileTransaction::write(const fs::path& file, std::string content, std::string description, bool backupWhenMissing)
{
    Operation op;
    op.kind = Kind::Write;
    op.target = file;
    op.content = std::move(content);
    op.description = std::move(description);
    op.backupWhenMissing = backupWhenMissing;
    ops_.push_back(std::move(op));
}

void FileTransaction::rename(const fs::path& from, const fs::path& to, std::string description)
{
    Operation op;
    op.kind = Kind::Rename;
    op.source = from;
    op.target = to;
    op.description = std::move(description);
    ops_.push_back(std::move(op));
}

void FileTransaction::copy(const fs::path& from, const fs::path& to, std::string description, bool replaceExisting)
{
    Operation op;
    op.kind = Kind::Copy;
    op.source = from;
    op.target = to;
    op.description = std::move(description);
    op.replaceExisting = replaceExisting;
    ops_.push_back(std::move(op));
}

std::vector<std::string> FileTransaction::summary() const
{
    std::vector<std::string> out;
    for (const auto& op : ops_) {
        std::string line = verb(op.kind);
        if (op.kind == Kind::Write)
            line += " " + pathToUtf8(op.target);
        else
            line += " " + pathToUtf8(op.source) + " -> " + pathToUtf8(op.target);
        if (!op.description.empty())
            line += ": " + op.description;
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<std::string> FileTransaction::check() const
{
    std::vector<std::string> problems;
    std::error_code ec;
    for (const auto& op : ops_) {
        switch (op.kind) {
        case Kind::Write:
            if (fs::exists(op.target, ec) && !fs::is_regular_file(op.target, ec))
                problems.push_back(pathToUtf8(op.target) + " is not a file");
            else if (!fs::is_directory(op.target.parent_path(), ec))
                problems.push_back("folder not found: " + pathToUtf8(op.target.parent_path()));
            break;
        case Kind::Rename:
            if (!fs::is_regular_file(op.source, ec))
                problems.push_back("file not found: " + pathToUtf8(op.source));
            if (fs::exists(op.target, ec))
                problems.push_back("already exists: " + pathToUtf8(op.target));
            break;
        case Kind::Copy:
            if (!fs::is_regular_file(op.source, ec))
                problems.push_back("file not found: " + pathToUtf8(op.source));
            if (!fs::is_directory(op.target.parent_path(), ec))
                problems.push_back("folder not found: " + pathToUtf8(op.target.parent_path()));
            else if (fs::exists(op.target, ec) && (!op.replaceExisting || !fs::is_regular_file(op.target, ec)))
                problems.push_back("already exists: " + pathToUtf8(op.target));
            break;
        }
    }
    return problems;
}

void FileTransaction::apply()
{
    if (applied_)
        throw std::logic_error("transaction already applied");
    if (auto problems = check(); !problems.empty())
        throw std::runtime_error(name_ + ": " + join(problems, "; "));
    spdlog::info("{}: applying {} operation(s)", name_, ops_.size());
    size_t done = 0;
    try {
        for (auto& op : ops_) {
            std::error_code ec;
            switch (op.kind) {
            case Kind::Write: {
                auto current = readWhole(op.target);
                op.existedBefore = current.has_value();
                if (current && *current == op.content) {
                    op.skipped = true;
                    break;
                }
                if (op.existedBefore || op.backupWhenMissing)
                    makeBackup(op, stamp_, op.existedBefore);
                atomicWriteText(op.target, op.content);
                spdlog::info("{}: wrote {}", name_, pathToUtf8(op.target));
                break;
            }
            case Kind::Rename:
                if (fs::exists(op.target, ec)) // appeared since check()
                    throw std::runtime_error("already exists: " + pathToUtf8(op.target));
                fs::rename(op.source, op.target);
                spdlog::info("{}: renamed {} to {}", name_, pathToUtf8(op.source), pathToUtf8(op.target));
                break;
            case Kind::Copy:
                op.existedBefore = fs::exists(op.target, ec);
                if (op.existedBefore) {
                    if (!op.replaceExisting)
                        throw std::runtime_error("already exists: " + pathToUtf8(op.target));
                    makeBackup(op, stamp_, true);
                }
                atomicCopy(op.source, op.target);
                spdlog::info("{}: copied {} to {}", name_, pathToUtf8(op.source), pathToUtf8(op.target));
                break;
            }
            op.applied = true;
            ++done;
        }
    } catch (const std::exception& e) {
        spdlog::error("{}: {}; undoing the {} completed operation(s)", name_, e.what(), done);
        undo(done);
        throw;
    }
    applied_ = true;
}

void FileTransaction::rollback()
{
    if (!applied_)
        return;
    spdlog::info("{}: rolling back", name_);
    undo(ops_.size());
    applied_ = false;
}

void FileTransaction::undo(size_t count)
{
    // Undo in reverse order; keep going after a failure so as much as possible is restored.
    for (size_t i = count; i-- > 0;) {
        auto& op = ops_[i];
        if (!op.applied)
            continue;
        try {
            std::error_code ec;
            switch (op.kind) {
            case Kind::Write:
            case Kind::Copy:
                if (op.skipped)
                    break;
                if (op.existedBefore)
                    atomicCopy(op.backup, op.target);
                else
                    fs::remove(op.target, ec);
                break;
            case Kind::Rename:
                fs::rename(op.target, op.source);
                break;
            }
            op.applied = false;
            spdlog::info("{}: undid {} {}", name_, verb(op.kind), pathToUtf8(op.target));
        } catch (const std::exception& e) {
            spdlog::error("{}: could not undo {} {}: {} (backup: {})", name_, verb(op.kind), pathToUtf8(op.target),
                          e.what(), pathToUtf8(op.backup));
        }
    }
}

std::vector<fs::path> FileTransaction::changedFiles() const
{
    std::vector<fs::path> out;
    for (const auto& op : ops_)
        if (op.applied && !op.skipped)
            out.push_back(op.target);
    return out;
}

std::vector<fs::path> FileTransaction::backups() const
{
    std::vector<fs::path> out;
    for (const auto& op : ops_)
        if (!op.backup.empty())
            out.push_back(op.backup);
    return out;
}

} // namespace fo4mx
