#include "engine/ReferenceIndex.hpp"

#include "engine/FieldModel.hpp"
#include "engine/Normalizer.hpp"
#include "engine/ParallelFor.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace fo4mx::refs {

std::vector<std::pair<FormKey, Signature>> referencesOf(Session& session, const RecordInstance& record)
{
    Normalizer norm(session);
    auto fs = splitFields(norm.normalize(record), session.defs);
    auto out = collectRefs(fs);
    out.erase(std::remove_if(out.begin(), out.end(), [&](const auto& r) { return r.first == record.key; }), out.end());
    return out;
}

namespace {

bool containsObjectId(const Record& rec, uint32_t objectId)
{
    const uint8_t needle[3] = {uint8_t(objectId), uint8_t(objectId >> 8), uint8_t(objectId >> 16)};
    for (const auto& sr : rec.subrecords) {
        const auto& d = sr.data;
        if (d.size() < 3)
            continue;
        for (size_t i = 0; i + 3 <= d.size(); ++i)
            if (d[i] == needle[0] && d[i + 1] == needle[1] && d[i + 2] == needle[2])
                return true;
    }
    return false;
}

} // namespace

std::vector<Referrer> referencedBy(Session& session, FormKey target, ProgressSink& progress, unsigned threads)
{
    // Plugins that can name the target: the plugin that defines it and those listing it as a master.
    std::vector<uint8_t> canSee(session.plugins.size(), 0);
    for (const auto& p : session.plugins) {
        if (!p.scanned || !p.error.empty())
            continue;
        if (p.index == target.origin ||
            std::find(p.masters.begin(), p.masters.end(), target.origin) != p.masters.end())
            canSee[p.index] = 1;
    }
    std::vector<uint32_t> candidates;
    for (uint32_t i = 0; i < session.records.size(); ++i) {
        const auto& r = session.records[i];
        if (r.plugin < canSee.size() && canSee[r.plugin] && !(r.key == target))
            candidates.push_back(i);
    }

    std::vector<Referrer> out;
    std::mutex mutex;
    std::atomic<size_t> done{0};
    progress.stage("Finding references");
    parallelFor(
        candidates.size(),
        [&](size_t k) {
            const auto& inst = session.records[candidates[k]];
            try {
                Record rec = session.loadRecord(inst);
                if (containsObjectId(rec, target.objectId & 0xFFFFFF)) {
                    Normalizer norm(session);
                    auto fs = splitFields(norm.normalize(rec, inst.plugin), session.defs);
                    for (const auto& [to, via] : collectRefs(fs))
                        if (to == target) {
                            std::lock_guard lock(mutex);
                            out.push_back({candidates[k], via});
                        }
                }
            } catch (const std::exception&) {
                // An unreadable record is reported by the scan and the definition check.
            }
            const size_t n = ++done;
            if ((n & 0xFFF) == 0 || n == candidates.size())
                progress.progress(n, candidates.size(), "records checked");
        },
        progress, threads);
    std::sort(out.begin(), out.end(), [](const Referrer& a, const Referrer& b) {
        return a.record != b.record ? a.record < b.record : a.via < b.via;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Referrer& a, const Referrer& b) { return a.record == b.record && a.via == b.via; }),
              out.end());
    return out;
}

} // namespace fo4mx::refs
