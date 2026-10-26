#pragma once

#include "engine/CompilationEngine.hpp"
#include "engine/ConflictEngine.hpp"
#include "engine/DependencyEngine.hpp"
#include "engine/Session.hpp"
#include "engine/ValidationEngine.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fo4mx {

enum class ReportKind : uint8_t { Conflict, Asset, Dependency, Compilation, Validation };
const char* reportKindName(ReportKind k);

struct ReportContext {
    const Session* session = nullptr;
    const ConflictReport* conflicts = nullptr;
    const DependencyReport* dependencies = nullptr;
    const CompiledOutput* compiled = nullptr;
    const ValidationReport* validation = nullptr;
    size_t maxRows = 5000; // per table in HTML output
};

class ReportGenerator {
public:
    static std::string html(ReportKind kind, const ReportContext& ctx);
    static std::string json(ReportKind kind, const ReportContext& ctx);

    // Writes <dir>/<Kind>Report.html and .json for every report that has data.
    static std::vector<std::filesystem::path> writeAll(const ReportContext& ctx, const std::filesystem::path& dir);
};

} // namespace fo4mx
