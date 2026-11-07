#pragma once

#include "engine/Session.hpp"
#include "util/Progress.hpp"

#include <vector>

namespace fo4mx {

// Record-level references for any version of any record, on demand (xEdit's "Referenced By").
// The dependency analysis keeps references only for the winning version of compilable records;
// these functions read whatever is asked, from the plugins on disk, without storing an index.
namespace refs {

// What one version of a record refers to: (target, subrecord that holds it), FormIDs resolved
// through its plugin's masters. Uses the record definitions; bytes they do not describe give none.
std::vector<std::pair<FormKey, Signature>> referencesOf(Session& session, const RecordInstance& record);

// One record version that refers to the target.
struct Referrer {
    uint32_t record = 0; // index into Session::records
    Signature via;       // the subrecord holding the reference
};

// Every version of every record, in every scanned plugin that can see the target (the plugin that
// defines it and plugins with it as a master), that refers to `target`. Records whose bytes do not
// contain the target's object id are skipped without decoding.
std::vector<Referrer> referencedBy(Session& session, FormKey target, ProgressSink& progress = nullProgress(),
                                   unsigned threads = 0);

} // namespace refs
} // namespace fo4mx
