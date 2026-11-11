#pragma once

#include <string>

namespace fo4mx {

class Session;
struct EmittedRecord;

// Read-only evidence for cells kept out because of their precombine or previs data: reads the
// cell's previs file (vis\<plugin>\<cell>.uvd, or the file named by RVIS) and looks for its
// precombined meshes, then compares the file's object IDs with the cell's previs (XPRI) and
// combined (XCRI) reference lists, reading the IDs in the numbering of each plugin that could
// own them (the winning cell record's, the cell's own, the previs file's cell's). Nothing is
// changed: the result is reported so that what these files hold can be established on real data.
std::string checkPrevis(Session& session, const EmittedRecord& cell);

} // namespace fo4mx
