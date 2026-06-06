// TypeScriptSymbolExtractor -- L1 regex-based TypeScript symbol extraction.
//
// Extracts exported declarations from TypeScript source files using
// brace-based scope tracking:
//   - export function / export default function   -> Function
//   - export class                                 -> Class
//   - export interface                             -> Interface
//   - export type NAME = ...                       -> TypeAlias
//   - export (const|let|var)                       -> Variable
//   - export { A, B as C }                         -> one HostSymbol per name / rebinding (Function)
//   - export namespace                             -> scope-only (qualifier prefix); no symbol emitted
//   - class bodies: (public|private|protected)? (static|async)? NAME(...)  -> Method
//
// Brace depth is tracked approximately: we strip `//` line comments, but
// avoid a full string/regex/template-literal parse. False positives are
// acceptable; false negatives are safety issues.  Multi-line block comments
// are handled by a single `inBlockComment` flag.
//
// TODO: full string-literal + template-literal brace tracking.
// TODO: `export default class` / `export default function` anonymous forms.

#include "TypeScriptSymbolExtractor.h"
#include "V8SourceScanner.h"

#include <cctype>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace topo::check {

namespace {

// Per-extractor ScopeEntry kept as a thin wrapper around the shared
// shape — symbol extractor only cares about isClass / isNamespace.
// The shared struct allows the booleans we don't use to stay false.
using v8scanner::ScopeEntry;

// Pull scanning primitives from the shared header.
using v8scanner::stripLineComment;
using v8scanner::countBraces;

std::string findEnclosingClass(const std::vector<ScopeEntry>& stack) {
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (it->isClass) return it->name;
    }
    return "";
}

/// Build qualified name joining namespace and class scopes with '.'.
/// e.g. ns=["orders"], class="Processor", method="run"  ->  "orders.Processor.run"
std::string buildQualifiedName(const std::vector<ScopeEntry>& stack, const std::string& simple) {
    std::string qname;
    for (const auto& s : stack) {
        if (s.isClass || s.isNamespace) {
            if (!qname.empty()) qname += ".";
            qname += s.name;
        }
    }
    if (!qname.empty()) qname += ".";
    qname += simple;
    return qname;
}

/// Map a TypeScript `public|private|protected` modifier to Topo Visibility.
std::optional<Visibility> mapVisModifier(const std::string& mod) {
    if (mod == "public") return Visibility::Public;
    if (mod == "private") return Visibility::Private;
    if (mod == "protected") return Visibility::Protected;
    return std::nullopt;
}

/// Wrapper around v8scanner::stripBlockCommentState that preserves the
/// historical inBlockComment-by-reference param name in this file.
std::string stripBlockCommentState(const std::string& line, bool& inBlockComment) {
    return v8scanner::stripBlockCommentState(line, inBlockComment);
}

} // namespace

std::vector<HostSymbol> TypeScriptSymbolExtractor::extractSymbols(const std::string& filePath) {
    std::vector<HostSymbol> result;
    std::ifstream file(filePath);
    if (!file.is_open()) return result;

    // Top-level export regexes (match only at brace depth 0 + optional
    // enclosing namespace).
    static const std::regex exportFunctionRegex(
        R"(^\s*export\s+(?:default\s+)?(?:async\s+)?function\s+(\w+)\s*\()");
    static const std::regex exportClassRegex(
        R"(^\s*export\s+(?:default\s+)?(?:abstract\s+)?class\s+(\w+))");
    static const std::regex exportInterfaceRegex(
        R"(^\s*export\s+interface\s+(\w+))");
    static const std::regex exportTypeAliasRegex(
        R"(^\s*export\s+type\s+(\w+)\s*=)");
    static const std::regex exportVarRegex(
        R"(^\s*export\s+(?:const|let|var)\s+(\w+))");
    static const std::regex exportNamespaceRegex(
        R"(^\s*export\s+(?:namespace|module)\s+(\w+))");
    static const std::regex exportListRegex(
        R"(^\s*export\s*\{([^}]*)\})");
    // CommonJS single-symbol export:
    //   module.exports.X = ...
    //   exports.X = ...
    static const std::regex cjsNamedExportRegex(
        R"(^\s*(?:module\.)?exports\.(\w+)\s*=)");
    // CommonJS bulk export: module.exports = { A, B: C, D }
    static const std::regex cjsBulkExportRegex(
        R"(^\s*module\.exports\s*=\s*\{([^}]*)\})");
    // Class member: optional vis, optional static/async, then NAME(
    static const std::regex methodRegex(
        R"(^\s*(?:(public|private|protected)\s+)?(?:static\s+)?(?:async\s+)?(\w+)\s*\()");

    std::vector<ScopeEntry> scopeStack;
    int braceDepth = 0;
    bool inBlockComment = false;
    std::string line;
    int lineNum = 0;

    while (std::getline(file, line)) {
        ++lineNum;

        // stripBlockCommentState now returns the code text that precedes a
        // mid-line `/*` (and is string/`//`-aware), so we must NOT discard the
        // line just because block-comment state is now open — the retained
        // prefix may carry a real declaration. Only skip when nothing remains.
        std::string processed = stripBlockCommentState(line, inBlockComment);
        processed = stripLineComment(processed);
        if (processed.find_first_not_of(" \t\r") == std::string::npos) continue;

        int lineStartDepth = braceDepth;

        // Only inspect declarations when we're at top level or inside a
        // namespace/class scope (i.e. not inside a function body).
        bool atDeclarableScope = true;
        for (const auto& s : scopeStack) {
            if (!s.isClass && !s.isNamespace) { atDeclarableScope = false; break; }
        }

        if (atDeclarableScope && lineStartDepth == (scopeStack.empty() ? 0 : scopeStack.back().braceDepth + 1)) {
            // --- Top-level-ish declarations ---
            std::smatch m;
            if (std::regex_search(processed, m, exportFunctionRegex)) {
                HostSymbol sym;
                sym.simpleName = m[1].str();
                sym.qualifiedName = buildQualifiedName(scopeStack, sym.simpleName);
                sym.kind = HostSymbolKind::Function;
                sym.file = filePath;
                sym.line = lineNum;
                sym.hostVisibility = Visibility::Public;
                result.push_back(std::move(sym));
            } else if (std::regex_search(processed, m, exportClassRegex)) {
                std::string name = m[1].str();
                HostSymbol sym;
                sym.simpleName = name;
                sym.qualifiedName = buildQualifiedName(scopeStack, name);
                sym.kind = HostSymbolKind::Class;
                sym.file = filePath;
                sym.line = lineNum;
                sym.hostVisibility = Visibility::Public;
                result.push_back(std::move(sym));
                // If line contains `{`, push a class scope.
                auto [opens, _] = countBraces(processed);
                if (opens > 0) {
                    // ScopeEntry layout: {name, braceDepth, isClass, isFunction, isNamespace}.
                    scopeStack.push_back({name, braceDepth, /*isClass=*/true, /*isFunction=*/false, /*isNamespace=*/false});
                }
            } else if (std::regex_search(processed, m, exportInterfaceRegex)) {
                HostSymbol sym;
                sym.simpleName = m[1].str();
                sym.qualifiedName = buildQualifiedName(scopeStack, sym.simpleName);
                sym.kind = HostSymbolKind::Interface;
                sym.file = filePath;
                sym.line = lineNum;
                sym.hostVisibility = Visibility::Public;
                result.push_back(std::move(sym));
            } else if (std::regex_search(processed, m, exportTypeAliasRegex)) {
                HostSymbol sym;
                sym.simpleName = m[1].str();
                sym.qualifiedName = buildQualifiedName(scopeStack, sym.simpleName);
                sym.kind = HostSymbolKind::TypeAlias;
                sym.file = filePath;
                sym.line = lineNum;
                sym.hostVisibility = Visibility::Public;
                result.push_back(std::move(sym));
            } else if (std::regex_search(processed, m, exportVarRegex)) {
                HostSymbol sym;
                sym.simpleName = m[1].str();
                sym.qualifiedName = buildQualifiedName(scopeStack, sym.simpleName);
                sym.kind = HostSymbolKind::Variable;
                sym.file = filePath;
                sym.line = lineNum;
                sym.hostVisibility = Visibility::Public;
                result.push_back(std::move(sym));
            } else if (std::regex_search(processed, m, exportNamespaceRegex)) {
                // Namespaces are scope containers, not host symbols -- they
                // parallel C++ `namespace` and Java `package`, not Java
                // `class`. Emitting them would force .topo to declare every
                // intermediate scope as a type, which .topo's own namespace
                // syntax already handles structurally. The namespace scope
                // is still pushed so inner function/class qualifiedNames
                // carry the `Outer.Inner.` prefix.
                std::string name = m[1].str();
                auto [opens, _] = countBraces(processed);
                if (opens > 0) {
                    scopeStack.push_back({name, braceDepth, /*isClass=*/false, /*isFunction=*/false, /*isNamespace=*/true});
                }
            } else if (std::regex_search(processed, m, exportListRegex)) {
                // `export { A, B as C }` -- one symbol per exported name.
                std::string inner = m[1].str();
                std::regex itemRegex(R"((\w+)(?:\s+as\s+(\w+))?)");
                auto begin = std::sregex_iterator(inner.begin(), inner.end(), itemRegex);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it) {
                    std::string exported = (*it)[2].matched ? (*it)[2].str() : (*it)[1].str();
                    if (exported.empty()) continue;
                    HostSymbol sym;
                    sym.simpleName = exported;
                    sym.qualifiedName = buildQualifiedName(scopeStack, exported);
                    sym.kind = HostSymbolKind::Function;
                    sym.file = filePath;
                    sym.line = lineNum;
                    sym.hostVisibility = Visibility::Public;
                    result.push_back(std::move(sym));
                }
            } else if (std::regex_search(processed, m, cjsBulkExportRegex)) {
                // `module.exports = { A, B: _b, C }` -- one symbol per
                // listed key. Kept kind=Function as a scaffold fallback
                // (matches the handling of `export { ... }` above).
                std::string inner = m[1].str();
                std::regex itemRegex(R"((\w+)(?:\s*:\s*\w+)?)");
                auto begin = std::sregex_iterator(inner.begin(), inner.end(), itemRegex);
                auto end = std::sregex_iterator();
                for (auto it = begin; it != end; ++it) {
                    std::string exported = (*it)[1].str();
                    if (exported.empty()) continue;
                    HostSymbol sym;
                    sym.simpleName = exported;
                    sym.qualifiedName = buildQualifiedName(scopeStack, exported);
                    sym.kind = HostSymbolKind::Function;
                    sym.file = filePath;
                    sym.line = lineNum;
                    sym.hostVisibility = Visibility::Public;
                    result.push_back(std::move(sym));
                }
            } else if (std::regex_search(processed, m, cjsNamedExportRegex)) {
                // `module.exports.X = ...` / `exports.X = ...`
                HostSymbol sym;
                sym.simpleName = m[1].str();
                sym.qualifiedName = buildQualifiedName(scopeStack, sym.simpleName);
                sym.kind = HostSymbolKind::Function;
                sym.file = filePath;
                sym.line = lineNum;
                sym.hostVisibility = Visibility::Public;
                result.push_back(std::move(sym));
            } else if (!scopeStack.empty() && scopeStack.back().isClass) {
                // --- Class body: method declarations ---
                std::smatch mm;
                if (std::regex_search(processed, mm, methodRegex)) {
                    std::string visMod = mm[1].matched ? mm[1].str() : "";
                    std::string name = mm[2].str();
                    // Skip reserved keywords / control-flow that match NAME(
                    static const std::vector<std::string> keywords = {
                        "if", "for", "while", "switch", "return", "catch", "constructor"};
                    bool isKeyword = false;
                    for (const auto& k : keywords) if (name == k) { isKeyword = true; break; }

                    if (name == "constructor") {
                        HostSymbol sym;
                        sym.simpleName = name;
                        sym.qualifiedName = buildQualifiedName(scopeStack, name);
                        sym.kind = HostSymbolKind::Constructor;
                        sym.enclosingClass = findEnclosingClass(scopeStack);
                        sym.file = filePath;
                        sym.line = lineNum;
                        sym.hostVisibility = mapVisModifier(visMod).value_or(Visibility::Public);
                        result.push_back(std::move(sym));
                    } else if (!isKeyword) {
                        HostSymbol sym;
                        sym.simpleName = name;
                        sym.qualifiedName = buildQualifiedName(scopeStack, name);
                        sym.kind = HostSymbolKind::Method;
                        sym.enclosingClass = findEnclosingClass(scopeStack);
                        sym.file = filePath;
                        sym.line = lineNum;
                        sym.hostVisibility = mapVisModifier(visMod).value_or(Visibility::Public);
                        result.push_back(std::move(sym));
                    }
                }
            }
        }

        // Update brace depth after handling the line so that `class Foo {`
        // pushes the class scope with the OLD depth (opened on this line).
        auto [opens, closes] = countBraces(processed);
        braceDepth += opens;
        braceDepth -= closes;
        if (braceDepth < 0) braceDepth = 0;

        // Pop scopes whose braceDepth is now past the boundary.
        while (!scopeStack.empty() && braceDepth <= scopeStack.back().braceDepth) {
            scopeStack.pop_back();
        }
    }

    return result;
}

} // namespace topo::check
