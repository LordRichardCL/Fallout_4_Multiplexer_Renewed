#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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
    // IDs that point to no record today (a master index past the plugin's master list, or a record that
    // exists in no plugin): the game finds nothing for them now, so each is given an ID that finds nothing
    // in the output either (its own range, above every record it holds) instead of refusing the file.
    size_t nowhere = 0;
    std::vector<std::string> nowhereExamples; // the first few, for the report
};
PrevisTranslation translatePrevis(Session& session, const CompiledOutput& out, const EmittedRecord& cell);

// The same for any plugin that will hold the cell's winning record: `express` gives a record's FormID in
// that plugin's numbering (false: it cannot be expressed); IDs that point to no record today get IDs from
// `nextUnused` up (at most `maxObject`) in the plugin's own index `ownIndex`. `cell` supplies the key and
// RVIS (whose file it is).
PrevisTranslation translatePrevisInto(Session& session, const EmittedRecord& cell, const std::string& target,
                                      const std::function<bool(FormKey, uint32_t&)>& express, uint32_t ownIndex,
                                      uint32_t nextUnused, uint32_t maxObject);

// The records a cell's own previs file names, read as the game reads them today (the winning plugin's
// numbering); IDs that point to no record are left out (translatePrevis gives them unused IDs). Used to
// give the plugin that will hold the cell the masters the translated file needs. False with `problem`
// set when the file is there but cannot be read; true with no keys when the cell has no file of its own.
bool previsRecords(Session& session, const EmittedRecord& cell, std::vector<FormKey>& keys, std::string& problem);

// Read-only evidence for cells kept out because of their precombine or previs data: reads the
// cell's previs file (vis\<plugin>\<cell>.uvd, or the file named by RVIS) and looks for its
// precombined meshes, then compares the file's object IDs with the cell's previs (XPRI) and
// combined (XCRI) reference lists, reading the IDs in the numbering of each plugin that could
// own them (the winning cell record's, the cell's own, the previs file's cell's). Nothing is
// changed: the result is reported so that what these files hold can be established on real data.
std::string checkPrevis(Session& session, const EmittedRecord& cell);

} // namespace fo4mx
