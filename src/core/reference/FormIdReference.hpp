#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fo4mx {

class Session;
struct ValidationReport;

// Editor IDs of the records in Fallout4.esm and the six DLC masters, from an independent tool's
// export (resources/Fallout4_reference_editorids.bin, built by tools/build_formid_reference.py).
// Read-only reference data: used to cross-check this app's own reading of those plugins and to name
// base-game FormIDs. Never used to decide anything about a build.
class FormIdReference {
public:
    static constexpr const char* kFileName = "Fallout4_reference_editorids.bin";

    struct Plugin {
        std::string name;
        uint32_t sourceRows = 0;  // every record the source listed for it (with or without Editor ID)
        uint32_t maxObjectId = 0;
    };
    struct Entry {
        std::string_view editorId;
        bool baseGameOverride = false; // a DLC's override of the Fallout4.esm record with this id
    };

    // Reads the file; throws std::runtime_error when it is not a valid reference file.
    static FormIdReference load(const std::filesystem::path& file);
    // The same, or nothing when the file does not exist.
    static std::optional<FormIdReference> tryLoad(const std::filesystem::path& file);

    const std::vector<Plugin>& plugins() const { return plugins_; }
    size_t size() const { return keys_.size(); }
    const std::string& sourceSha256() const { return sha256_; } // of the JSON it was built from (hex)

    // Every Editor ID listed for (plugin, lower 24 bits of the FormID); empty when none.
    std::vector<Entry> find(std::string_view plugin, uint32_t objectId) const;
    // Every row of one plugin, by object id: (object id, entry).
    std::vector<std::pair<uint32_t, Entry>> entries(std::string_view plugin) const;

private:
    int pluginIndex(std::string_view name) const;

    std::vector<Plugin> plugins_;
    std::vector<uint32_t> keys_;    // plugin << 24 | object id, sorted
    std::vector<uint32_t> offsets_; // parallel: text offset, bit 31 = base-game override
    std::string text_;
    std::string sha256_;
};

// The comparison of this app's scan of the base-game masters with the reference.
struct ReferenceComparison {
    struct PluginResult {
        std::string plugin;
        size_t compared = 0;  // reference rows for records the plugin defines (overrides left out)
        size_t matched = 0;   // scanned with the same Editor ID
        size_t missing = 0;   // no record with that FormID defined by the plugin in the scan
        size_t different = 0; // scanned with another Editor ID
        size_t extra = 0;     // scanned records with an Editor ID the reference does not list
        std::vector<std::string> samples; // first differences, for the report
    };
    std::vector<PluginResult> plugins; // only plugins present in the scan
    bool agrees() const;
};
ReferenceComparison compareWithReference(const FormIdReference& reference, const Session& session);
// Adds the comparison to a report as "Reference Check" findings: one Info line per plugin that
// agrees, a Warning per plugin that does not (likely another game version, or a reading problem).
void reportReferenceComparison(const ReferenceComparison& comparison, ValidationReport& report);

} // namespace fo4mx
