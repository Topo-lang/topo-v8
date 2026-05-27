#ifndef TOPO_CHECK_TYPESCRIPTIMPORTEXTRACTOR_H
#define TOPO_CHECK_TYPESCRIPTIMPORTEXTRACTOR_H

#include "topo/Check/ImportExtractor.h"

#include <string>
#include <vector>

namespace topo::check {

/// L1 regex-based TypeScript import extractor.
///
/// Recognizes:
///   - ES modules: `import X from "path"`, `import { A, B as C } from "path"`,
///                 `import * as N from "path"`, `import "path"` (side-effect),
///                 `import type { T } from "path"`
///   - Re-export:  `export { X } from "path"`, `export * from "path"`,
///                 `export * as N from "path"`
///   - CommonJS:   `const x = require("path")`, `const { a } = require("path")`
///
/// Dynamic `import(...)` is intentionally NOT matched here — the CallSite
/// extractor catches it via a synthetic `__dynamic_import__` callee.
///
/// Paths are kept as-is (no resolution of relative paths).
class TypeScriptImportExtractor : public ImportExtractor {
public:
    std::vector<HostImport> extractImports(const std::string& filePath) override;
};

} // namespace topo::check

#endif // TOPO_CHECK_TYPESCRIPTIMPORTEXTRACTOR_H
