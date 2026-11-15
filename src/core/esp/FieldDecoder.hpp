#pragma once

#include "esp/Record.hpp"
#include "esp/RecordDefinitions.hpp"

#include <string>
#include <optional>
#include <span>
#include <string_view>
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
    std::vector<FormIdSlot> probableSlots; // FormIDs only if the value names an existing record
    std::vector<LocalizedField> localized;
    std::vector<HeuristicSpan> heuristic;
    std::vector<DecodeIssue> issues;
    // String values of script properties (VMAD) that look like a plugin file name ("Mod.esp"):
    // scripts can pass them to GetFormFromFile, so the record depends on that plugin's file name.
    std::vector<std::string> scriptPluginNames;
    // The same strings with the script and (top-level) property they are in.
    struct ScriptString {
        std::string script;
        std::string property;
        std::string value;
    };
    std::vector<ScriptString> scriptStrings;

    bool slotCovers(uint32_t sub, uint32_t offset) const; // true if [offset, offset+4) overlaps a slot
};

// Locate every FormID and localised string in a record's subrecords.
DecodedLayout decodeLayout(const RecordDefinitions& defs, Signature record, const std::vector<Subrecord>& subs);

// Replaces the string value of one top-level property of one script in a VMAD subrecord, when its value is
// `expected` (names compared without case and without a trailing NUL some tools write). Returns the new
// VMAD, or nothing when the script, property or value is not there or the data does not read. VMAD holds
// counts, never byte sizes or offsets, so the string can change length; everything after it is copied.
std::optional<std::vector<uint8_t>> replaceScriptString(std::span<const uint8_t> vmad, std::string_view script,
                                                        std::string_view property, std::string_view expected,
                                                        const std::string& replacement);

} // namespace fo4mx
