#pragma once

#include "engine/FieldModel.hpp"

#include <string>
#include <vector>

namespace fo4mx {

enum class CellStatus : uint8_t {
    Base,     // value of the defining (first) plugin
    Same,     // identical to the result
    Winning,  // supplies the result (green)
    Merged,   // supplies the result although it is not the last plugin (yellow)
    Replaced, // differs from the result (red)
    Absent,
};
const char* cellStatusName(CellStatus s);

struct FieldAnalysis {
    std::string key;
    std::string label;
    std::vector<size_t> changers; // chain indices whose value differs from the base
    bool hard = false;            // two or more changers disagree
    bool listField = false;
    size_t source = 0;            // chain index that supplies the merged value
    bool fromNonWinner = false;   // merged value differs from the last plugin's value
    std::vector<CellStatus> cells; // per chain entry
};

struct ChainAnalysis {
    std::vector<FieldAnalysis> fields; // only fields some plugin changed
    std::vector<size_t> changers;      // chain indices that changed anything
    std::vector<size_t> itms;          // chain indices identical to the base
    bool hard = false;
    bool winnerLosesChanges = false;   // last-plugin-wins discards someone's edit
};

struct MergeOptions {
    bool mergeCompatible = true; // false = last plugin wins
    int forcedWinner = -1;       // chain index chosen by the user, or -1
};

struct MergeResult {
    FieldSet fields;
    ChainAnalysis analysis;
    bool merged = false; // result differs from the last plugin's record
    std::vector<size_t> contributors;
};

class MergeEngine {
public:
    // chain[0] is the base (first definition), chain.back() the load-order winner.
    static ChainAnalysis analyse(const std::vector<FieldSet>& chain);
    static MergeResult merge(const std::vector<FieldSet>& chain, const MergeOptions& options = {});
};

} // namespace fo4mx
