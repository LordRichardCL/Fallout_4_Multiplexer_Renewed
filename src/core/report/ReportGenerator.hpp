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

// Rich: modern CSS for browsers. Preview/Print: the CSS subset Qt's rich text
// engine understands (dark for the in-app preview, light for PDF output).
enum class ReportStyle : uint8_t { Rich, Preview, Print };

struct ReportContext {
    const Session* session = nullptr;
    const ConflictReport* conflicts = nullptr;
    const DependencyReport* dependencies = nullptr;
    const CompiledOutput* compiled = nullptr;
    const ValidationReport* validation = nullptr;
    size_t maxRows = 5000; // per table in HTML output
    ReportStyle style = ReportStyle::Rich;
};

class ReportGenerator {
public:
    static std::string html(ReportKind kind, const ReportContext& ctx);
    static std::string json(ReportKind kind, const ReportContext& ctx);
    // Plain text with aligned columns, for reading in any text editor.
    static std::string text(ReportKind kind, const ReportContext& ctx);

    // Writes <dir>/<Kind>Report.html, .json and .txt for every report that has data.
    static std::vector<std::filesystem::path> writeAll(const ReportContext& ctx, const std::filesystem::path& dir);
};

} // namespace fo4mx
