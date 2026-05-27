// TypeScriptImportExtractor -- L1 regex-based TypeScript import extraction.
//
// Parses ES module `import` / `export ... from` statements and CommonJS
// `require(...)` assignments. Keeps paths as-is; downstream does any path
// resolution. Dynamic `import()` is intentionally NOT treated as a static
// import — the CallSite extractor emits a synthetic `__dynamic_import__`
// callee for those.
//
// Handles `/* ... */` block comments and `// ...` line comments to avoid
// false positives inside documentation blocks or commented-out code.

#include "TypeScriptImportExtractor.h"
#include "V8SourceScanner.h"

#include <cctype>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace topo::check {

namespace {

/// Conservatively classify a module path's unsafe level. TypeScript doesn't
/// have its own UnsafeCatalog yet (unlike Python/Rust/C++), so we inline a
/// minimal set of known-risky paths here. Extend once TypeScriptUnsafeCatalog
/// is introduced.
/// TODO: move this to a TypeScriptUnsafeCatalog sibling of PythonUnsafeCatalog.
UnsafeLevel classifyTypeScriptImport(const std::string& path) {
    // Node built-ins considered Escape (shell / dynamic loading).
    if (path == "child_process" || path == "vm" || path == "module")
        return UnsafeLevel::Escape;
    // Node built-ins considered System (fs, network, process).
    if (path == "fs" || path == "fs/promises" || path == "net" ||
        path == "http" || path == "https" || path == "dgram" ||
        path == "dns" || path == "tls" || path == "os" ||
        path == "process" || path == "cluster" || path == "worker_threads")
        return UnsafeLevel::System;
    return UnsafeLevel::Safe;
}

// stripBlockCommentState / stripLineComment moved to V8SourceScanner.h
// (shared across all 5 V8-family extractors). using-decls keep call
// sites below unchanged.
using v8scanner::stripBlockCommentState;
using v8scanner::stripLineComment;

} // anonymous namespace

std::vector<HostImport> TypeScriptImportExtractor::extractImports(const std::string& filePath) {
    std::vector<HostImport> results;
    std::ifstream file(filePath);
    if (!file.is_open()) return results;

    // ES module imports. Capture the module path in group 1.
    //   import defaultExport from "module"
    //   import { A, B as C } from "module"
    //   import * as N from "module"
    //   import type { T } from "module"
    //   import defaultExport, { A } from "module"
    static const std::regex esImportRegex(
        R"(^\s*import\s+(?:type\s+)?[^;]*?from\s*['"]([^'"]+)['"])");
    // Side-effect-only: `import "module";`
    static const std::regex esSideEffectRegex(
        R"(^\s*import\s+['"]([^'"]+)['"])");
    // Re-export from: `export { X } from "module"`, `export * from "module"`,
    // `export * as N from "module"`, `export type { T } from "module"`.
    static const std::regex esReexportRegex(
        R"(^\s*export\s+(?:type\s+)?(?:\*(?:\s+as\s+\w+)?|\{[^}]*\})\s*from\s*['"]([^'"]+)['"])");
    // CommonJS require assigned to a binding:
    //   const x = require("path")
    //   const { a, b } = require("path")
    //   let x = require("path")
    //   var x = require("path")
    static const std::regex cjsRequireRegex(
        R"((?:const|let|var)\s+[^=]+=\s*require\s*\(\s*['"]([^'"]+)['"]\s*\))");

    std::string line;
    int lineNum = 0;
    bool inBlockComment = false;

    while (std::getline(file, line)) {
        ++lineNum;

        std::string processed = stripBlockCommentState(line, inBlockComment);
        if (inBlockComment) continue;
        processed = stripLineComment(processed);

        // Skip blank lines and comment-only lines.
        if (processed.find_first_not_of(" \t\r\n") == std::string::npos) continue;

        auto emit = [&](const std::string& path) {
            HostImport imp;
            imp.normalizedPath = path;
            imp.file = filePath;
            imp.line = lineNum;
            imp.unsafeLevel = classifyTypeScriptImport(path);
            results.push_back(std::move(imp));
        };

        std::smatch m;

        // `import ... from "path"` — must try this BEFORE the side-effect form
        // because both start with `import`.
        if (std::regex_search(processed, m, esImportRegex)) {
            emit(m[1].str());
            continue;
        }
        // `import "path"` side-effect
        if (std::regex_search(processed, m, esSideEffectRegex)) {
            emit(m[1].str());
            continue;
        }
        // `export { X } from "path"`, `export * from "path"`, etc.
        if (std::regex_search(processed, m, esReexportRegex)) {
            emit(m[1].str());
            continue;
        }
        // CommonJS `require()` — can appear anywhere on a line, scan all.
        {
            std::string searchStr = processed;
            while (std::regex_search(searchStr, m, cjsRequireRegex)) {
                emit(m[1].str());
                searchStr = m.suffix().str();
            }
        }
    }

    return results;
}

} // namespace topo::check
