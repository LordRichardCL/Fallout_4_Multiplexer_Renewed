#pragma once

#include "db/Database.hpp"
#include "engine/ConflictEngine.hpp"
#include "engine/FieldModel.hpp"
#include "engine/Session.hpp"
#include "esp/FieldDecoder.hpp"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fo4mx {

// Record index entry of one plugin, independent of the session's plugin numbering.
struct CachedRecord {
    Signature sig;
    uint32_t flags = 0;
    uint32_t dataSize = 0;
    uint32_t rawFormId = 0;
    uint64_t offset = 0;
    uint64_t hash = 0;
    std::string editorId;
};

struct CachedPluginIndex {
    uint64_t fileHash = 0;
    std::vector<CachedRecord> records;
};

// Result of merging one override chain.
struct CachedMerge {
    FieldSet fields;
    bool merged = false;
    std::vector<uint32_t> contributors;            // chain indices
    std::vector<uint8_t> equalsInstance;           // result identical to chain[i]
    std::vector<std::vector<DecodeIssue>> issues;  // per chain entry
};

// Persistent cache that makes rescans and recompiles incremental. Entries are
// keyed by content signatures, so a changed plugin only invalidates the work
// that involves it. Thread-safe.
class BuildCache {
public:
    explicit BuildCache(const std::filesystem::path& file);
    static std::filesystem::path defaultPathFor(const std::filesystem::path& databasePath);

    std::optional<CachedPluginIndex> loadPluginIndex(const std::string& path, uint64_t size, int64_t modified,
                                                     uint64_t sample);
    void storePluginIndex(const std::string& path, uint64_t size, int64_t modified, uint64_t sample,
                          const CachedPluginIndex& index);

    // Generic content-addressed blobs: table is one of "conflicts", "merges", "edges".
    std::optional<std::vector<uint8_t>> get(const char* table, uint64_t key);
    void put(const char* table, uint64_t key, std::span<const uint8_t> blob);

    void begin();
    void commit();
    void clear();

    struct Counters {
        std::atomic<uint64_t> hits{0};
        std::atomic<uint64_t> misses{0};
    };
    Counters plugins, conflicts, merges, edges;
    void resetCounters();

private:
    std::mutex mutex_;
    db::Database db_;
};

// Hash of the first and last 64 KiB plus the size: cheap content check for large files.
uint64_t sampleFileHash(const std::filesystem::path& path, uint64_t size);
uint64_t fullFileHash(const std::filesystem::path& path);

// Content signature of an override chain: every instance's plugin, master list and record bytes.
uint64_t chainSignature(const Session& session, const FormGroup& group);

// Serialisation. FormKeys are stored by plugin name; deserialising fails
// (returns nullopt) when a referenced plugin no longer exists.
std::vector<uint8_t> serializeConflict(const Session& session, const ConflictEntry& entry);
std::optional<ConflictEntry> deserializeConflict(const Session& session, std::span<const uint8_t> blob);
std::vector<uint8_t> serializeMerge(const Session& session, const CachedMerge& merge);
std::optional<CachedMerge> deserializeMerge(const Session& session, std::span<const uint8_t> blob);
std::vector<uint8_t> serializeEdges(const Session& session, const std::vector<std::pair<FormKey, Signature>>& refs);
std::optional<std::vector<std::pair<FormKey, Signature>>> deserializeEdges(const Session& session,
                                                                            std::span<const uint8_t> blob);

// Conflict cell values reference forms through tokens so labels (which contain
// load-order FormIDs) are rendered against the current load order.
std::string formToken(FormKey key);
std::string renderFormTokens(const Session& session, const std::string& text);

} // namespace fo4mx
