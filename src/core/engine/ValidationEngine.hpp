#pragma once

#include "engine/CompilationEngine.hpp"
#include "engine/Session.hpp"
#include "esp/FieldDecoder.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace fo4mx {

struct ValidationIssue {
    Severity severity = Severity::Warning;
    std::string category; // Missing References, Broken FormIDs, Duplicate IDs, ...
    std::string message;
    std::string plugin;
    std::string formId;
};

struct ValidationReport {
    uint64_t checks = 0;
    uint64_t passed = 0;
    uint64_t warnings = 0;
    uint64_t errors = 0;
    std::vector<ValidationIssue> issues;

    bool critical() const { return errors > 0; }
    void pass(uint64_t n = 1)
    {
        checks += n;
        passed += n;
    }
    void fail(Severity s, std::string category, std::string message, std::string plugin = {}, std::string formId = {});
    void merge(const ValidationReport& other);
};

// Stage Validate: proves the compiled output is internally consistent before
// anything is written, and re-reads the written files afterwards.
class ValidationEngine {
public:
    static ValidationReport validate(const Session& session, const CompiledOutput& compiled,
                                     ProgressSink& progress = nullProgress());
    static ValidationReport validateWrittenFiles(const CompiledOutput& compiled,
                                                 const std::vector<std::filesystem::path>& files);
};

} // namespace fo4mx
