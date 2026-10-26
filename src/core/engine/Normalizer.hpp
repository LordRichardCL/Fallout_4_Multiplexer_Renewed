#pragma once

#include "engine/Session.hpp"
#include "esp/FieldDecoder.hpp"

#include <vector>

namespace fo4mx {

// A subrecord with every FormID replaced by its global FormKey, so values can
// be compared between plugins with different master lists.
struct NormSub {
    Signature sig;
    std::vector<uint8_t> bytes; // described FormID slots are zeroed
    std::vector<std::pair<uint32_t, FormKey>> refs;     // described FormIDs
    std::vector<std::pair<uint32_t, FormKey>> softRefs; // probable FormIDs in undescribed bytes (bytes kept raw)
    bool heuristic = false; // contains undescribed bytes

    bool operator==(const NormSub&) const = default;
};

// Equality that also treats soft references as equal when they resolve to the
// same form (their raw bytes may differ only in the master index).
bool equivalent(const NormSub& a, const NormSub& b);
bool equivalent(const std::vector<NormSub>& a, const std::vector<NormSub>& b);

struct NormRecord {
    FormKey key;
    Signature sig;
    PluginIndex plugin = kInvalidPlugin;
    uint32_t flags = 0;
    uint16_t formVersion = 0;
    std::vector<NormSub> subs;
    std::vector<DecodeIssue> issues;
};

class Normalizer {
public:
    explicit Normalizer(Session& session) : session_(session) {}

    NormRecord normalize(const RecordInstance& inst);
    NormRecord normalize(const Record& record, PluginIndex plugin);

private:
    Session& session_;
};

} // namespace fo4mx
