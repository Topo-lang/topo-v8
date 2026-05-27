#ifndef TOPO_CHECK_TYPESCRIPTCALLSITEEXTRACTOR_H
#define TOPO_CHECK_TYPESCRIPTCALLSITEEXTRACTOR_H

#include "topo/Check/CallSiteExtractor.h"

#include <string>
#include <vector>

namespace topo::check {

/// L1 regex-based TypeScript call site extractor.
///
/// Scans TypeScript source files for dangerous API calls using regex
/// patterns. Uses brace-depth scope tracking (mirrors TypeScriptSymbolExtractor)
/// to determine the caller function for each detected call site.
///
/// Recognized dangerous calls:
///   - eval(...), new Function(...)                  → Escape
///   - import(...)  (dynamic import)                 → Escape
///   - require(...)  (CommonJS dynamic require)      → Escape
///   - child_process.exec / execSync / spawn         → Escape
///   - fs.*  (file system)                           → System
///   - net.*, http.*, https.* (network)              → System
///   - process.exit, process.env writes              → System
///
/// Design: false positives are acceptable, false negatives are safety issues.
class TypeScriptCallSiteExtractor : public CallSiteExtractor {
public:
    std::vector<DetectedCallSite> extractCallSites(const std::string& filePath) override;
};

} // namespace topo::check

#endif // TOPO_CHECK_TYPESCRIPTCALLSITEEXTRACTOR_H
