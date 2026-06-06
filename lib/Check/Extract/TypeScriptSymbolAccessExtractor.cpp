// TypeScriptSymbolAccessExtractor — L1 regex extractor for TypeScript global writes.
//
// Strategy:
//   Pass 1: scan the file and collect module-level mutable globals — `let X`
//           and `var X` declarations at top-level (outside any function or
//           class body). `const X` is NOT collected because its binding is
//           immutable (even though the referent object may be mutated — that's
//           object state, not a binding rewrite).
//
//   Pass 2: emit SymbolAccess{isWrite=true} for writes inside function bodies:
//             - simple assignment  `name = ...`
//             - compound assignment `name += / -= / *= / /= / %= / **= / <<= / >>= / &= / |= / ^=`
//             - subscript write    `name[key] = ...`
//             - attribute write    `name.attr = ...`
//             - well-known global writes: `globalThis.*`, `window.*`,
//               `global.*`, `process.env.*`
//             - `Object.assign(globalThis | window | global, ...)`
//
// Conservative posture: false positives acceptable (user can ignore
// non-parallel functions); false negatives lose checker value. Instance
// writes via `this.x = y` are NOT flagged — `this.x` is object state.

#include "TypeScriptSymbolAccessExtractor.h"
#include "V8SourceScanner.h"

#include <cctype>
#include <fstream>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

namespace topo::check {

namespace {

using v8scanner::ScopeEntry;
using v8scanner::stripLineComment;
using v8scanner::stripBlockCommentState;
using v8scanner::maskStringLiterals;
using v8scanner::countBraces;

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

const std::unordered_set<std::string>& reservedNames() {
    static const std::unordered_set<std::string> kws = {
        "if", "else", "for", "while", "do", "switch", "case", "break",
        "continue", "return", "throw", "try", "catch", "finally",
        "function", "class", "const", "let", "var", "new", "delete",
        "typeof", "instanceof", "void", "yield", "await", "interface",
        "type", "enum", "namespace", "module", "declare", "export",
        "import", "default", "extends", "implements", "abstract", "async",
        "static", "public", "private", "protected", "readonly", "as", "is",
        "true", "false", "null", "undefined", "this", "super",
    };
    return kws;
}

bool isReserved(const std::string& name) {
    return reservedNames().count(name) > 0;
}

struct ScopePattern {
    std::regex regex;
    bool isClass;
    bool isFunction;
};

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

/// Pass 1: collect module-level mutable globals — top-level `let` / `var`
/// declarations outside any function / class body. `const` bindings are
/// immutable so they aren't flagged even if the referenced object is mutated.
std::unordered_set<std::string> collectGlobals(const std::string& filePath) {
    std::unordered_set<std::string> globals;
    std::ifstream file(filePath);
    if (!file.is_open()) return globals;

    static const auto scopePatterns = buildScopePatterns();
    // Module-level mutable binding: `let NAME` / `var NAME` (not `const`).
    // Also `export let NAME` / `export var NAME`.
    static const std::regex mutableDeclRegex(
        R"(^\s*(?:export\s+)?(?:let|var)\s+(\w+))");

    std::vector<ScopeEntry> scopeStack;
    int braceDepth = 0;
    bool inBlockComment = false;
    std::string line;

    while (std::getline(file, line)) {
        // Retain code before a mid-line `/*` so brace/scope state stays in sync.
        std::string processed = stripBlockCommentState(line, inBlockComment);
        processed = stripLineComment(processed);
        if (processed.find_first_not_of(" \t\r\n") == std::string::npos) continue;

        int lineStartDepth = braceDepth;

        // Try scope-opening patterns first — these change scope.
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

        // Module-level mutable declaration: scope stack empty AND we did not
        // just open a scope on this line.
        if (!openedScope && scopeStack.empty()) {
            std::smatch m;
            if (std::regex_search(processed, m, mutableDeclRegex)) {
                std::string name = m[1].str();
                if (!isReserved(name)) globals.insert(name);
            }
        }

        auto [opens, closes] = countBraces(processed);
        braceDepth += opens;
        braceDepth -= closes;
        if (braceDepth < 0) braceDepth = 0;
        while (!scopeStack.empty() && braceDepth <= scopeStack.back().braceDepth) {
            scopeStack.pop_back();
        }
    }

    return globals;
}

} // anonymous namespace

std::vector<SymbolAccess> TypeScriptSymbolAccessExtractor::extractSymbolAccesses(
    const std::string& filePath) {
    std::vector<SymbolAccess> results;

    auto globals = collectGlobals(filePath);

    std::ifstream file(filePath);
    if (!file.is_open()) return results;

    static const auto scopePatterns = buildScopePatterns();
    // Class-method shorthand, including getter/setter, generator, `#private`
    // and computed names, with an optional `: ReturnType` annotation — so the
    // method body opens a function scope and writes inside it are attributed.
    static const std::regex methodRegex(
        R"(^\s*(?:(?:public|private|protected)\s+)?(?:static\s+)?(?:readonly\s+)?(?:async\s+)?(?:(?:get|set)\s+)?\*?\s*(#?\w+|\[[^\]]+\])\s*\([^)]*\)\s*(?::\s*[^{]+?)?\{)");
    // Class-field arrow method: `name = (args) => { ... }` etc. (with an
    // optional `: Type` annotation that may be a function type containing an
    // inner `=>`; anchored on the trailing `=> {`). These class-field arrows
    // were not recognized as a function scope, so global writes inside them
    // were dropped.
    static const std::regex arrowMethodRegex(
        R"(^\s*(?:(?:public|private|protected)\s+)?(?:static\s+)?(?:readonly\s+)?(#?\w+)\s*(?::\s*.+)?=\s*(?:async\s+)?(?:\([^)]*\)|\w+)\s*(?::\s*[^=]+?)?=>\s*\{\s*$)");
    // Well-known global-root writes: `globalThis.x = ...`, `window.foo = ...`,
    // `process.env.X = ...`, `global.bar = ...`.
    static const std::regex wellKnownWriteRegex(
        R"(\b(globalThis|window|global|process)\.([\w.]+)\s*(=[^=]|[+\-*/%&|^]=|<<=|>>=|\*\*=))");
    // Object.assign(globalThis|window|global, ...)
    static const std::regex objectAssignGlobalRegex(
        R"(\bObject\.assign\s*\(\s*(globalThis|window|global)\b)");

    std::vector<ScopeEntry> scopeStack;
    int braceDepth = 0;
    bool inBlockComment = false;
    bool inTemplate = false;  // multi-line template-literal state
    std::string line;
    int lineNum = 0;

    while (std::getline(file, line)) {
        ++lineNum;

        // Retain code before a mid-line `/*` so brace/scope state stays in sync.
        std::string processed = stripBlockCommentState(line, inBlockComment);
        processed = stripLineComment(processed);
        // A line wholly inside a multi-line template literal carries no live
        // declarations; mask it (which clears it) so it neither matches a
        // global write nor desyncs scope tracking.
        if (!inTemplate &&
            processed.find_first_not_of(" \t\r\n") == std::string::npos) continue;

        int lineStartDepth = braceDepth;
        std::string masked = maskStringLiterals(processed, inTemplate);

        // Scope bookkeeping.
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
            bool matched = std::regex_search(processed, m, methodRegex) ||
                           std::regex_search(processed, m, arrowMethodRegex);
            if (matched) {
                std::string name = m[1].str();
                if (!name.empty() && !isReserved(name)) {
                    scopeStack.push_back({name, lineStartDepth,
                                          /*isClass=*/false, /*isFunction=*/true});
                    openedScope = true;
                }
            }
        }

        bool insideFunction = false;
        for (const auto& s : scopeStack) {
            if (s.isFunction) { insideFunction = true; break; }
        }

        if (insideFunction) {
            std::string callerName = buildCallerName(scopeStack);

            // --- Well-known global-root writes ---
            {
                std::string searchStr = masked;
                std::smatch m;
                while (std::regex_search(searchStr, m, wellKnownWriteRegex)) {
                    std::string root = m[1].str();
                    std::string sub = m[2].str();
                    // Word boundary check before match: ensure previous char
                    // isn't an identifier character (would mean we matched a
                    // substring).
                    size_t absPos = static_cast<size_t>(m.position(0)) +
                                    (masked.size() - searchStr.size());
                    if (absPos > 0) {
                        char prev = masked[absPos - 1];
                        if (std::isalnum(static_cast<unsigned char>(prev)) ||
                            prev == '_' || prev == '$' || prev == '.') {
                            searchStr = m.suffix().str();
                            continue;
                        }
                    }

                    SymbolAccess access;
                    access.function = callerName;
                    access.symbol = root + "." + sub;
                    access.isWrite = true;
                    access.file = filePath;
                    access.line = lineNum;
                    results.push_back(std::move(access));

                    searchStr = m.suffix().str();
                }
            }

            // --- Object.assign(globalThis|window|global, ...) ---
            {
                std::smatch m;
                if (std::regex_search(masked, m, objectAssignGlobalRegex)) {
                    SymbolAccess access;
                    access.function = callerName;
                    access.symbol = m[1].str();
                    access.isWrite = true;
                    access.file = filePath;
                    access.line = lineNum;
                    results.push_back(std::move(access));
                }
            }

            // --- Collected module-level globals ---
            for (const auto& name : globals) {
                size_t pos = 0;
                while (pos < masked.size()) {
                    size_t found = masked.find(name, pos);
                    if (found == std::string::npos) break;

                    // Word boundary BEFORE
                    bool leftOK = true;
                    if (found > 0) {
                        char prev = masked[found - 1];
                        if (std::isalnum(static_cast<unsigned char>(prev)) ||
                            prev == '_' || prev == '$') leftOK = false;
                        // `this.name = ...` / `obj.name = ...` is instance
                        // state, not a module global write.
                        if (prev == '.') leftOK = false;
                    }
                    size_t end = found + name.size();
                    bool rightOK = true;
                    if (end < masked.size()) {
                        char nxt = masked[end];
                        if (std::isalnum(static_cast<unsigned char>(nxt)) ||
                            nxt == '_' || nxt == '$') rightOK = false;
                    }
                    if (!leftOK || !rightOK) {
                        pos = found + 1;
                        continue;
                    }

                    size_t after = end;
                    while (after < masked.size() &&
                           (masked[after] == ' ' || masked[after] == '\t'))
                        ++after;

                    bool isWrite = false;
                    if (after < masked.size()) {
                        char c = masked[after];
                        // Simple `=` (not `==` / `===`).
                        if (c == '=' &&
                            (after + 1 >= masked.size() || masked[after + 1] != '=')) {
                            isWrite = true;
                        }
                        // Compound arithmetic: += -= *= /= %= &= |= ^=
                        if (!isWrite && after + 1 < masked.size() &&
                            masked[after + 1] == '=') {
                            if (c == '+' || c == '-' || c == '*' || c == '/' ||
                                c == '%' || c == '&' || c == '|' || c == '^') {
                                isWrite = true;
                            }
                        }
                        // Compound 3-char: **=, <<=, >>=, &&=, ||=, ??=
                        if (!isWrite && after + 2 < masked.size() &&
                            masked[after + 2] == '=') {
                            if ((c == '*' && masked[after + 1] == '*') ||
                                (c == '<' && masked[after + 1] == '<') ||
                                (c == '>' && masked[after + 1] == '>') ||
                                (c == '&' && masked[after + 1] == '&') ||
                                (c == '|' && masked[after + 1] == '|') ||
                                (c == '?' && masked[after + 1] == '?')) {
                                isWrite = true;
                            }
                        }
                        // Subscript write: `name[ ... ] = expr`
                        if (!isWrite && c == '[') {
                            size_t depth = 1;
                            size_t j = after + 1;
                            while (j < masked.size() && depth > 0) {
                                if (masked[j] == '[') ++depth;
                                else if (masked[j] == ']') --depth;
                                ++j;
                            }
                            if (depth == 0) {
                                while (j < masked.size() &&
                                       (masked[j] == ' ' || masked[j] == '\t'))
                                    ++j;
                                if (j < masked.size() && masked[j] == '=' &&
                                    (j + 1 >= masked.size() || masked[j + 1] != '=')) {
                                    isWrite = true;
                                }
                            }
                        }
                        // Attribute write: `name.attr = expr`
                        if (!isWrite && c == '.') {
                            size_t j = after;
                            while (j < masked.size() && masked[j] != '=' &&
                                   masked[j] != ';') {
                                ++j;
                            }
                            if (j < masked.size() && masked[j] == '=' &&
                                (j + 1 >= masked.size() || masked[j + 1] != '=')) {
                                if (j == 0 ||
                                    (masked[j - 1] != '<' && masked[j - 1] != '>' &&
                                     masked[j - 1] != '!' && masked[j - 1] != '=')) {
                                    isWrite = true;
                                }
                            }
                        }
                    }

                    if (isWrite) {
                        SymbolAccess access;
                        access.function = callerName;
                        access.symbol = name;
                        access.isWrite = true;
                        access.file = filePath;
                        access.line = lineNum;
                        results.push_back(std::move(access));
                        // One write per global per line is enough.
                        break;
                    }
                    pos = found + name.size();
                }
            }
        }

        // Count braces on the masked text so a `{`/`}` inside a (possibly
        // multi-line) template literal does not desync scope depth.
        auto [opens, closes] = countBraces(masked);
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
