#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fo4mx {

// "YYYYMMDD-HHMMSS" in local time, the stamp every backup this app writes carries.
std::string backupTimestamp();

// "<file>.multiplexer-backup-<stamp>", or with "-2", "-3", ... appended when that name is taken:
// an existing backup is never overwritten.
std::filesystem::path uniqueBackupPath(const std::filesystem::path& file, const std::string& stamp);

// Writes the whole file to "<file>.multiplexer-tmp" beside it, then renames it over the file, so a
// failure leaves either the old file or the new one, never a partial file. Throws on failure.
void atomicWriteText(const std::filesystem::path& file, std::string_view text);

// A plan of file operations applied all or nothing.
//
// Nothing touches the disk until apply(). apply() checks every operation first (sources exist,
// destinations that must be new are free), then backs up each file it will replace (unique names,
// never overwriting an existing backup) and applies the operations in order. If any step fails,
// everything already done is undone in reverse order and the error is rethrown. After a successful
// apply(), rollback() undoes the whole transaction (for a later step that fails); backups stay on disk.
class FileTransaction {
public:
    enum class Kind { Write, Rename, Copy };
    struct Operation {
        Kind kind = Kind::Write;
        std::filesystem::path target;      // file written, rename destination, copy destination
        std::filesystem::path source;      // rename / copy source
        std::string content;               // Write
        std::string description;           // for the summary shown before changes
        bool backupWhenMissing = false;    // Write: an empty backup records "there was no file"
        bool replaceExisting = false;      // Copy: an existing destination may be replaced (backed up)
        // Set by apply():
        bool existedBefore = false;
        bool applied = false;
        bool skipped = false;              // Write with identical content: nothing to do
        std::filesystem::path backup;
    };

    explicit FileTransaction(std::string name);

    void write(const std::filesystem::path& file, std::string content, std::string description,
               bool backupWhenMissing = false);
    void rename(const std::filesystem::path& from, const std::filesystem::path& to, std::string description);
    void copy(const std::filesystem::path& from, const std::filesystem::path& to, std::string description,
              bool replaceExisting = false);

    const std::string& name() const { return name_; }
    bool empty() const { return ops_.empty(); }
    const std::vector<Operation>& operations() const { return ops_; }
    // One line per planned operation, for the confirmation before anything changes.
    std::vector<std::string> summary() const;
    // Problems that would stop apply() (missing sources, destinations already taken). Empty = ready.
    std::vector<std::string> check() const;

    void apply();
    void rollback();
    bool applied() const { return applied_; }

    // Files that were changed (written, renamed to, copied to), and the backups made.
    std::vector<std::filesystem::path> changedFiles() const;
    std::vector<std::filesystem::path> backups() const;

private:
    void undo(size_t count);

    std::string name_;
    std::string stamp_;
    std::vector<Operation> ops_;
    bool applied_ = false;
};

} // namespace fo4mx
