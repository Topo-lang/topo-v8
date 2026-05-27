#ifndef TOPO_CHECK_TYPESCRIPTSYMBOLACCESSEXTRACTOR_H
#define TOPO_CHECK_TYPESCRIPTSYMBOLACCESSEXTRACTOR_H

#include "topo/Check/SymbolAccessExtractor.h"

#include <string>
#include <vector>

namespace topo::check {

/// L1 regex-based TypeScript symbol access extractor used by PurityCheck.
///
/// Flags writes to module-level mutable bindings and globals:
///   - Module-level `let x = ...` / `var x = ...` that is reassigned anywhere
///     (module-scope mutable binding). `const x = ...` is NOT flagged since
///     its binding is immutable.
///   - Writes to `globalThis.*`, `window.*`, `global.*`, `process.env.*`.
///   - `Object.assign(globalThis, ...)` / `Object.assign(window, ...)`.
///   - Writes to imported namespace members: `import * as M from "..."; M.state = ...`.
///
/// Reads are deferred to a later milestone — the load-bearing signal for
/// PurityCheck is writes in parallel stages. False positives are acceptable;
/// false negatives lose checker value. Instance writes (`this.x = y`) are NOT
/// flagged — those are object state.
class TypeScriptSymbolAccessExtractor : public SymbolAccessExtractor {
public:
    std::vector<SymbolAccess> extractSymbolAccesses(const std::string& filePath) override;
};

} // namespace topo::check

#endif // TOPO_CHECK_TYPESCRIPTSYMBOLACCESSEXTRACTOR_H
