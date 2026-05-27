// TypeScriptCallEdgeExtractor — L1 regex extractor for TypeScript caller→callee edges.
//
// Mirrors PythonCallEdgeExtractor but uses brace-depth scope tracking
// (TypeScript uses braces, not indentation). For each identifier call inside
// a function body, emits a CallEdge with:
//   - caller qualified by the nearest enclosing class (e.g. "Cls.method"),
//     or just the function name at module scope ("foo")
//   - callee = the call target token. Bare `bar(...)` → "bar". Member calls
//     `obj.bar(...)` are emitted twice — once with the simple name "bar" and
//     once with the dotted form converted to "obj::bar" (mirroring the Python
//     convention of `::` as namespace separator for VisibilityCheck).
//
// Filter rules:
//   - Skip JavaScript / TypeScript keywords and control-flow tokens
//   - Skip the function definition tokens themselves
//   - Skip dunder / symbol-like names inside arguments (best effort)

#include "TypeScriptCallEdgeExtractor.h"
#include "V8SourceScanner.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace topo::check {

namespace {

/// Result of resolving a destructured ES import.
struct ImportedName {
    std::string module;   ///< Module-name hint derived from the import path stem
                          ///< (e.g. "./app" -> "app").
    std::string origName; ///< Name as exported by the module — equals `local`
                          ///< unless the user wrote `name as alias`.
};

using v8scanner::ScopeEntry;
using v8scanner::stripLineComment;
using v8scanner::stripBlockCommentState;
using v8scanner::maskStringLiterals;
using v8scanner::countBraces;

/// Derive a "module namespace" hint from the file path's stem.
/// `src/app.ts` → `app`. Used to synthesize file-qualified call edges so that
/// `.topo` declarations like `namespace app { ... }` can match TypeScript
/// sources that have no source-level namespace.
std::string fileNamespaceHint(const std::string& filePath) {
    namespace fs = std::filesystem;
    fs::path p(filePath);
    return p.stem().string();
}

std::string buildCallerName(const std::vector<ScopeEntry>& scopeStack) {
    std::string className;
    std::string funcName;
    for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
        if (it->isFunction && funcName.empty()) {
            funcName = it->name;
        } else if (it->isClass && className.empty()) {
            className = it->name;
        }
    }
    if (funcName.empty()) return "<module>";
    if (!className.empty()) return className + "." + funcName;
    return funcName;
}

/// JavaScript / TypeScript keywords and common global constructors that look
/// like calls but are not user-defined function references.
const std::unordered_set<std::string>& tsKeywordsAndBuiltins() {
    static const std::unordered_set<std::string> kws = {
        // Control flow and statements
        "if", "else", "for", "while", "do", "switch", "case", "break",
        "continue", "return", "throw", "try", "catch", "finally", "with",
        "in", "of", "new", "delete", "typeof", "instanceof", "void",
        "yield", "await",
        // Declarations
        "function", "class", "const", "let", "var", "interface", "type",
        "enum", "namespace", "module", "declare", "export", "import",
        "default", "extends", "implements", "abstract", "async", "static",
        "public", "private", "protected", "readonly", "as", "is", "satisfies",
        // Literals
        "true", "false", "null", "undefined", "this", "super",
        // Common built-in constructors that aren't user-defined
        "Array", "Object", "Number", "String", "Boolean", "Date", "RegExp",
        "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError",
        "Map", "Set", "WeakMap", "WeakSet", "Promise", "Symbol", "BigInt",
        "JSON", "Math",
        // Common built-in functions
        "parseInt", "parseFloat", "isNaN", "isFinite", "encodeURI",
        "decodeURI", "encodeURIComponent", "decodeURIComponent",
        "console",
    };
    return kws;
}

bool isKeywordOrBuiltin(const std::string& name) {
    return tsKeywordsAndBuiltins().count(name) > 0;
}

/// Scope-opening regex patterns matched only when we need to push a new scope.
struct ScopePattern {
    std::regex regex;
    bool isClass;
    bool isFunction;
};

/// Derive a module hint from an ES import path.
/// `./app` -> `app`; `../utils/strings` -> `strings`; `lib` -> `lib`.
std::string moduleHintFromPath(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path p(path);
    return p.stem().string();
}

/// Pre-scan the file for destructured ES imports of the form
///   `import { foo, bar as baz } from "./mod"`
/// and `import defaultName from "./mod"` (default-only). Returns a map from
/// the local binding name to `{module, origName}` so the call-edge emitter
/// can rewrite a bare `foo()` call into the qualified `<mod>::foo` edge that
/// `.topo` declarations match against.
///
/// Re-export forms (`export { x } from "./mod"`) are intentionally ignored
/// — they don't introduce a callable local binding.
std::unordered_map<std::string, ImportedName>
collectDestructuredImports(const std::string& filePath) {
    std::unordered_map<std::string, ImportedName> result;
    std::ifstream file(filePath);
    if (!file.is_open()) return result;

    static const std::regex namedImportRegex(
        R"(^\s*import\s+(?:type\s+)?(?:[A-Za-z_$][\w$]*\s*,\s*)?\{\s*([^}]+?)\s*\}\s+from\s+['"]([^'"]+)['"])");
    static const std::regex defaultPlusNamed(
        R"(^\s*import\s+(?:type\s+)?([A-Za-z_$][\w$]*)\s*,\s*\{[^}]*\}\s+from\s+['"]([^'"]+)['"])");
    static const std::regex defaultOnly(
        R"(^\s*import\s+(?:type\s+)?([A-Za-z_$][\w$]*)\s+from\s+['"]([^'"]+)['"])");
    static const std::regex specifierRegex(
        R"(([A-Za-z_$][\w$]*)\s*(?:as\s+([A-Za-z_$][\w$]*))?)");

    bool inBlockComment = false;
    std::string line;
    while (std::getline(file, line)) {
        std::string processed = stripBlockCommentState(line, inBlockComment);
        if (inBlockComment) continue;
        processed = stripLineComment(processed);

        std::smatch m;
        if (std::regex_search(processed, m, namedImportRegex)) {
            std::string specifiers = m[1].str();
            std::string module = moduleHintFromPath(m[2].str());

            // Split specifiers on `,`, then parse each `name` or `name as alias`.
            std::string buf;
            buf.reserve(specifiers.size());
            for (char c : specifiers) {
                if (c == ',') {
                    std::smatch sm;
                    if (std::regex_search(buf, sm, specifierRegex)) {
                        std::string orig = sm[1].str();
                        std::string local = sm[2].matched ? sm[2].str() : orig;
                        result[local] = {module, orig};
                    }
                    buf.clear();
                } else {
                    buf.push_back(c);
                }
            }
            if (!buf.empty()) {
                std::smatch sm;
                if (std::regex_search(buf, sm, specifierRegex)) {
                    std::string orig = sm[1].str();
                    std::string local = sm[2].matched ? sm[2].str() : orig;
                    result[local] = {module, orig};
                }
            }
        }
        // `import default, { ... } from "mod"` — also bind the default.
        if (std::regex_search(processed, m, defaultPlusNamed)) {
            std::string local = m[1].str();
            std::string module = moduleHintFromPath(m[2].str());
            result[local] = {module, "default"};
        } else if (std::regex_search(processed, m, defaultOnly)) {
            std::string local = m[1].str();
            std::string module = moduleHintFromPath(m[2].str());
            result[local] = {module, "default"};
        }
    }

    return result;
}

std::vector<ScopePattern> buildScopePatterns() {
    std::vector<ScopePattern> sp;
    sp.push_back({std::regex(R"(^\s*(?:export\s+(?:default\s+)?)?(?:async\s+)?function\s*\*?\s*(\w+)\s*\()"),
                  false, true});
    sp.push_back({std::regex(R"(^\s*(?:export\s+(?:default\s+)?)?(?:abstract\s+)?class\s+(\w+))"),
                  true, false});
    sp.push_back({std::regex(R"(^\s*(?:export\s+)?(?:const|let|var)\s+(\w+)\s*=\s*(?:async\s+)?(?:\([^)]*\)|\w+)\s*=>\s*\{)"),
                  false, true});
    sp.push_back({std::regex(R"(^\s*(?:export\s+)?(?:const|let|var)\s+(\w+)\s*=\s*(?:async\s+)?function\s*\*?\s*\w*\s*\()"),
                  false, true});
    return sp;
}

} // anonymous namespace

std::vector<CallEdge> TypeScriptCallEdgeExtractor::extractCallEdges(const std::string& filePath) {
    std::vector<CallEdge> results;
    std::ifstream file(filePath);
    if (!file.is_open()) return results;

    static const auto scopePatterns = buildScopePatterns();
    // Method shorthand inside a class body: `NAME(args) { ... }`.
    static const std::regex methodRegex(
        R"(^\s*(?:(?:public|private|protected)\s+)?(?:static\s+)?(?:async\s+)?(\w+)\s*\([^)]*\)\s*\{)");
    // Object-member function: `NAME: function(...)` or `NAME: (args) => { ... }`.
    static const std::regex objectMemberFnRegex(
        R"(^\s*(\w+)\s*:\s*(?:async\s+)?function\s*\*?\s*\w*\s*\()");
    static const std::regex objectMemberArrowRegex(
        R"(^\s*(\w+)\s*:\s*(?:async\s+)?(?:\([^)]*\)|\w+)\s*=>\s*\{)");
    // Call target: optional dotted chain + `(`. Group 1 captures the full
    // callee (with dots).
    static const std::regex callRegex(
        R"(([A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)*)\s*\()");

    const std::string nsHint = fileNamespaceHint(filePath);
    const auto destructured = collectDestructuredImports(filePath);

    std::vector<ScopeEntry> scopeStack;
    int braceDepth = 0;
    bool inBlockComment = false;
    std::string line;
    int lineNum = 0;

    while (std::getline(file, line)) {
        ++lineNum;

        std::string processed = stripBlockCommentState(line, inBlockComment);
        if (inBlockComment) continue;
        processed = stripLineComment(processed);
        if (processed.find_first_not_of(" \t\r\n") == std::string::npos) continue;

        int lineStartDepth = braceDepth;

        // Try scope-opening patterns. The `{` on this line pushes depth by 1
        // on the same line, so the scope's braceDepth is the pre-line depth.
        bool openedScope = false;
        for (const auto& sp : scopePatterns) {
            std::smatch m;
            if (std::regex_search(processed, m, sp.regex)) {
                scopeStack.push_back({m[1].str(), lineStartDepth,
                                      sp.isClass, sp.isFunction});
                openedScope = true;
                break;
            }
        }
        if (!openedScope && !scopeStack.empty() && scopeStack.back().isClass) {
            std::smatch m;
            if (std::regex_search(processed, m, methodRegex)) {
                std::string name = m[1].str();
                if (!isKeywordOrBuiltin(name)) {
                    scopeStack.push_back({name, lineStartDepth,
                                          /*isClass=*/false, /*isFunction=*/true});
                    openedScope = true;
                }
            }
        }
        if (!openedScope) {
            std::smatch m;
            if (std::regex_search(processed, m, objectMemberFnRegex)) {
                scopeStack.push_back({m[1].str(), lineStartDepth,
                                      /*isClass=*/false, /*isFunction=*/true});
                openedScope = true;
            } else if (std::regex_search(processed, m, objectMemberArrowRegex)) {
                scopeStack.push_back({m[1].str(), lineStartDepth,
                                      /*isClass=*/false, /*isFunction=*/true});
                openedScope = true;
            }
        }

        // Determine whether we're inside a function scope for call emission.
        bool insideFunction = false;
        for (const auto& s : scopeStack) {
            if (s.isFunction) { insideFunction = true; break; }
        }

        if (insideFunction) {
            std::string callerName = buildCallerName(scopeStack);
            std::string scanLine = maskStringLiterals(processed);

            std::string remaining = scanLine;
            size_t absOffset = 0;
            while (true) {
                std::smatch m;
                if (!std::regex_search(remaining, m, callRegex)) break;
                std::string callee = m[1].str();
                size_t matchPos = absOffset + static_cast<size_t>(m.position(1));
                size_t matchLen = m[1].length();

                std::string simple = callee;
                auto dotPos = callee.rfind('.');
                if (dotPos != std::string::npos) {
                    simple = callee.substr(dotPos + 1);
                }

                bool skip = false;
                if (simple.empty() ||
                    (!std::isalpha(static_cast<unsigned char>(simple[0])) &&
                     simple[0] != '_' && simple[0] != '$')) {
                    skip = true;
                }
                if (!skip && isKeywordOrBuiltin(simple)) skip = true;
                if (!skip && dotPos != std::string::npos) {
                    std::string head = callee.substr(0, callee.find('.'));
                    if (isKeywordOrBuiltin(head)) skip = true;
                }

                // Word boundary check before the match: ensure the previous
                // char is not alphanumeric / `_` / `$` (otherwise we matched
                // a substring of a larger identifier).
                if (!skip && matchPos > 0) {
                    char prev = scanLine[matchPos - 1];
                    if (std::isalnum(static_cast<unsigned char>(prev)) ||
                        prev == '_' || prev == '$' || prev == '.') {
                        skip = true;
                    }
                }

                // Skip the line's own scope-opening identifier — e.g. the
                // `function foo(` itself should not emit a "caller calls foo"
                // edge. The scope patterns already pushed the new scope so
                // check if this match's callee matches the newly-pushed
                // function name.
                if (!skip && openedScope && !scopeStack.empty() &&
                    scopeStack.back().isFunction && simple == scopeStack.back().name) {
                    // Only skip if the match starts at or before the scope
                    // pattern's name position — best-effort.
                    size_t firstNonWs = scanLine.find_first_not_of(" \t");
                    if (matchPos <= firstNonWs + 32) {
                        skip = true;
                    }
                }

                if (!skip) {
                    // Convert dotted form `obj.method` → `obj::method`.
                    std::string canonicalCallee = callee;
                    {
                        std::string converted;
                        for (size_t k = 0; k < canonicalCallee.size(); ++k) {
                            if (canonicalCallee[k] == '.') converted += "::";
                            else converted += canonicalCallee[k];
                        }
                        canonicalCallee = std::move(converted);
                    }

                    CallEdge edge;
                    edge.caller = callerName;
                    edge.callee = canonicalCallee;
                    edge.file = filePath;
                    edge.line = lineNum;
                    results.push_back(edge);

                    if (dotPos != std::string::npos && !simple.empty()) {
                        CallEdge simpleEdge;
                        simpleEdge.caller = callerName;
                        simpleEdge.callee = simple;
                        simpleEdge.file = filePath;
                        simpleEdge.line = lineNum;
                        results.push_back(simpleEdge);
                    }

                    if (!nsHint.empty()) {
                        CallEdge qualified;
                        qualified.caller = nsHint + "::" + callerName;
                        qualified.callee = nsHint + "::" + simple;
                        qualified.file = filePath;
                        qualified.line = lineNum;
                        results.push_back(std::move(qualified));
                    }

                    // Resolve destructured ES imports to their source-module
                    // qualified form. `import { helper } from "./app"` followed
                    // by a bare `helper()` produces an additional edge with
                    // callee `app::helper`, matching `.topo` declarations
                    // namespaced under `app`. Only fires for bare callees
                    // (no dotted prefix) — dotted forms already qualify
                    // themselves.
                    if (dotPos == std::string::npos) {
                        auto it = destructured.find(simple);
                        if (it != destructured.end()) {
                            CallEdge resolved;
                            resolved.caller = callerName;
                            resolved.callee = it->second.module + "::" + it->second.origName;
                            resolved.file = filePath;
                            resolved.line = lineNum;
                            results.push_back(resolved);

                            // Also emit a caller-namespaced variant so
                            // VisibilityCheck's caller-side filename-namespace
                            // shorthand still applies.
                            if (!nsHint.empty()) {
                                CallEdge resolvedNs;
                                resolvedNs.caller = nsHint + "::" + callerName;
                                resolvedNs.callee = it->second.module + "::" + it->second.origName;
                                resolvedNs.file = filePath;
                                resolvedNs.line = lineNum;
                                results.push_back(std::move(resolvedNs));
                            }
                        }
                    }
                }

                size_t advance = static_cast<size_t>(m.position(1)) + matchLen;
                if (advance == 0) advance = 1;
                remaining = remaining.substr(advance);
                absOffset += advance;
            }
        }

        // Update brace depth after processing the line.
        auto [opens, closes] = countBraces(processed);
        braceDepth += opens;
        braceDepth -= closes;
        if (braceDepth < 0) braceDepth = 0;

        while (!scopeStack.empty() && braceDepth <= scopeStack.back().braceDepth) {
            scopeStack.pop_back();
        }
    }

    return results;
}

} // namespace topo::check
