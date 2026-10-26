#pragma once

#include "esp/Record.hpp"
#include "esp/RecordDefinitions.hpp"

#include <string>
#include <vector>

namespace fo4mx {

enum class Severity : uint8_t { Info = 0, Warning = 1, Error = 2 };
const char* severityName(Severity s);

struct FormIdSlot {
    uint32_t sub = 0;    // subrecord index
    uint32_t offset = 0; // byte offset inside the subrecord
};

// Bytes that may contain FormIDs the definitions do not describe.
struct HeuristicSpan {
    uint32_t sub = 0;
    uint32_t begin = 0;
    uint32_t end = 0;
};

struct DecodeIssue {
    Severity severity = Severity::Info;
    Signature sub;
    std::string message;
};

struct LocalizedField {
    uint32_t sub = 0;
    StringFileKind file = StringFileKind::Strings;
};

struct DecodedLayout {
    std::vector<FormIdSlot> formIdSlots;
    std::vector<LocalizedField> localized;
    std::vector<HeuristicSpan> heuristic;
    std::vector<DecodeIssue> issues;

    bool slotCovers(uint32_t sub, uint32_t offset) const; // true if [offset, offset+4) overlaps a slot
};

// Locate every FormID and localised string in a record's subrecords.
DecodedLayout decodeLayout(const RecordDefinitions& defs, Signature record, const std::vector<Subrecord>& subs);

} // namespace fo4mx
