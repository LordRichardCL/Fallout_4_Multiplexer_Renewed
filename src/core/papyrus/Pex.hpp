#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fo4mx::pex {

// A call of Game.GetFormFromFile(aiFormID, asFilename) found in a compiled Fallout 4 script.
struct FormFromFileCall {
    bool formIdLiteral = false;   // the FormID argument is an integer constant
    int32_t formId = 0;
    size_t formIdOffset = 0;      // byte offset of the int32 constant in the file
    bool fileLiteral = false;     // the file name argument is a string constant
    uint16_t fileString = 0;      // string table index of that constant
    size_t fileStringOffset = 0;  // byte offset of the uint16 index in the file
};

// How a string constant is used by a script's code.
enum class StringUse : uint8_t {
    GetFormFromFile,   // file name argument of Game.GetFormFromFile
    IsPluginInstalled, // argument of Game.IsPluginInstalled (presence check)
    CallArgument,      // argument of any other function call
    Assignment,        // assigned to a variable or property
    Comparison,        // compared with something
    Concatenation,     // joined with other text
    Default,           // default value of a variable, property or struct member
    Other,             // any other instruction
};
const char* stringUseName(StringUse u);

// One use of a string constant.
struct StringUsage {
    uint16_t string = 0;
    StringUse use = StringUse::Other;
    size_t offset = 0;    // byte offset of the uint16 string index
    std::string function; // called function ("Game.IsPluginInstalled"), for calls
    std::string where;    // "Object.State.Function" containing the instruction
    // GetFormFromFile only:
    bool formIdLiteral = false;
    int32_t formId = 0;
    size_t formIdOffset = 0;
};

// What the relinker needs from a compiled script. The whole file is walked with the layout the
// Fallout 4 compiler writes (as read by Champollion and written by Caprica): every count, string
// index and opcode is checked, each object's recorded size must match what was read, and the walk
// must end exactly at the end of the file. Anything else and `ok` is false: the script is not touched.
struct Scan {
    bool ok = false;
    std::string error;
    std::vector<std::string> strings;
    size_t stringTableBegin = 0; // offset of the uint16 string count
    size_t stringTableEnd = 0;   // first byte after the last string
    std::vector<FormFromFileCall> getFormFromFile;
    // String constants used anywhere other than as a GetFormFromFile file name argument (per index).
    std::vector<bool> usedElsewhere;
    // Every use of every string constant, with its context.
    std::vector<StringUsage> uses;
};

Scan scan(const std::vector<uint8_t>& bytes);

// In-place change to bytes after the string table (offsets as in the original file).
struct Patch {
    size_t offset = 0;
    std::vector<uint8_t> bytes;
};

// Rebuilds the file with a new string table (`strings`, at least as many entries as the original,
// existing indices keep their meaning) and the patches applied. Everything else is copied unchanged.
std::vector<uint8_t> rewrite(const std::vector<uint8_t>& bytes, const Scan& scan, const std::vector<std::string>& strings,
                             const std::vector<Patch>& patches);

} // namespace fo4mx::pex
