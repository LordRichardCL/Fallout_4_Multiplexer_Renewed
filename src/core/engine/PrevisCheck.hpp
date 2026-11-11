#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "esp/FormId.hpp"

namespace fo4mx {

class Session;
struct EmittedRecord;
struct CompiledOutput;

// The data-relative path of a cell's own previs file: vis\<cell's plugin>\<cell FormID>.uvd.
std::string previsFilePath(const Session& session, FormKey cell);

// Whether a cell's previs file is its own (no RVIS, or RVIS naming the cell itself). An exterior cell
// can be in the previs file of another cell of its cluster (RVIS).
bool ownsPrevisFile(const EmittedRecord& cell);

// The previs file of a cell written to an output, translated (P2b). The game reads its FormIDs in the
// numbering of the plugin whose cell record wins; once the cell is written to an output, the output's
// record wins (outputs load last). Each FormID is resolved as the game reads it today (the winning
// plugin's numbering) and written as the output expresses that record.
struct PrevisTranslation {
    bool hasFile = false;  // the cell has its own previs file in the load order
    bool ok = true;        // every FormID could be expressed in the output
    std::string path;
    std::string problem;   // why not, when !ok
    std::vector<uint8_t> bytes;
    std::string source;
    std::string numbering; // today's winning plugin
    size_t formIds = 0;
    size_t changed = 0;
};
PrevisTranslation translatePrevis(Session& session, const CompiledOutput& out, const EmittedRecord& cell);

// Read-only evidence for cells kept out because of their precombine or previs data: reads the
// cell's previs file (vis\<plugin>\<cell>.uvd, or the file named by RVIS) and looks for its
// precombined meshes, then compares the file's object IDs with the cell's previs (XPRI) and
// combined (XCRI) reference lists, reading the IDs in the numbering of each plugin that could
// own them (the winning cell record's, the cell's own, the previs file's cell's). Nothing is
// changed: the result is reported so that what these files hold can be established on real data.
std::string checkPrevis(Session& session, const EmittedRecord& cell);

} // namespace fo4mx
