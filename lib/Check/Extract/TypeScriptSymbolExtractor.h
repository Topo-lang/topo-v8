#ifndef TOPO_CHECK_TYPESCRIPTSYMBOLEXTRACTOR_H
#define TOPO_CHECK_TYPESCRIPTSYMBOLEXTRACTOR_H

#include "topo/Check/SymbolExtractor.h"

#include <string>
#include <vector>

namespace topo::check {

/// Regex-based L1 TypeScript symbol extractor using brace-based scope tracking.
///
/// Provides a safety-net fallback when tsserver LSP is unavailable.
/// Extracts classes, interfaces, type aliases, functions, methods, and
/// exported variables by parsing top-level declarations and walking brace
/// depth to detect enclosing classes and namespaces.
///
/// Design: false positives acceptable, false negatives are safety issues.
class TypeScriptSymbolExtractor : public SymbolExtractor {
public:
    std::vector<HostSymbol> extractSymbols(const std::string& filePath) override;
};

} // namespace topo::check

#endif // TOPO_CHECK_TYPESCRIPTSYMBOLEXTRACTOR_H
