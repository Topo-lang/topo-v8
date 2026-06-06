// TypeScriptCallSiteExtractor -- L1 regex-based TypeScript call site extraction.
//
// Scans TypeScript source files for dangerous API calls using regex patterns.
// Uses brace-depth scope tracking (mirrors TypeScriptSymbolExtractor) to
// determine the caller function for each detected call site.
//
// This is a safety-net fallback when tsserver LSP is unavailable.
// Design: false positives acceptable, false negatives are safety issues.

#include "TypeScriptCallSiteExtractor.h"
#include "V8SourceScanner.h"
#include "topo/Check/CapabilityCatalog.h"

#include <cctype>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace topo::check {

namespace {

using v8scanner::ScopeEntry;
using v8scanner::stripLineComment;
using v8scanner::stripBlockCommentState;
using v8scanner::countBraces;

/// Build caller name from the scope stack: nearest enclosing class.method,
/// or just function name at module scope.
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

struct DangerousPattern {
    std::regex regex;
    std::string patternName;  // empty means derive from match
    UnsafeLevel level;
};

std::vector<DangerousPattern> buildPatterns() {
    std::vector<DangerousPattern> patterns;

    // Level 4: Language escape mechanisms
    patterns.push_back({
        std::regex(R"(\beval\s*\()"),
        "eval", UnsafeLevel::Escape
    });
    patterns.push_back({
        std::regex(R"(\bnew\s+Function\s*\()"),
        "Function", UnsafeLevel::Escape
    });
    // Dynamic import: `import(...)`. Emitted as __dynamic_import__ so the
    // downstream containment catalog can recognize it without colliding with
    // static-form ES imports.
    patterns.push_back({
        std::regex(R"(\bimport\s*\()"),
        "__dynamic_import__", UnsafeLevel::Escape
    });
    // CommonJS dynamic require — even inside non-external functions, this
    // is a containment violation. The Import extractor picks up the form
    // `const x = require(...)`; this pattern also catches bare `require(...)`
    // mid-expression.
    patterns.push_back({
        std::regex(R"(\brequire\s*\()"),
        "require", UnsafeLevel::Escape
    });
    // child_process.* — shell execution escape
    patterns.push_back({
        std::regex(R"(\bchild_process\.(exec|execSync|execFile|execFileSync|spawn|spawnSync|fork)\s*\()"),
        "", UnsafeLevel::Escape
    });
    // vm.* — sandboxed (but still dangerous) code eval
    patterns.push_back({
        std::regex(R"(\bvm\.(runInThisContext|runInNewContext|runInContext|compileFunction)\s*\()"),
        "", UnsafeLevel::Escape
    });

    // Level 1: System calls
    patterns.push_back({
        std::regex(R"(\bfs\.(readFileSync|writeFileSync|appendFileSync|unlinkSync|readFile|writeFile|appendFile|unlink|open|openSync|mkdir|mkdirSync|rmdir|rmdirSync|stat|statSync|access|accessSync|createReadStream|createWriteStream)\s*\()"),
        "", UnsafeLevel::System
    });
    patterns.push_back({
        std::regex(R"(\bnet\.(createServer|createConnection|connect)\s*\()"),
        "", UnsafeLevel::System
    });
    patterns.push_back({
        std::regex(R"(\bhttps?\.(request|get|createServer)\s*\()"),
        "", UnsafeLevel::System
    });
    patterns.push_back({
        std::regex(R"(\bprocess\.(exit|kill|abort|chdir)\s*\()"),
        "", UnsafeLevel::System
    });
    // `fetch(...)` is universally available in modern TS — treat as System.
    patterns.push_back({
        std::regex(R"(\bfetch\s*\()"),
        "fetch", UnsafeLevel::System
    });
    // XMLHttpRequest constructor
    patterns.push_back({
        std::regex(R"(\bnew\s+XMLHttpRequest\s*\()"),
        "XMLHttpRequest", UnsafeLevel::System
    });
    // WebSocket constructor
    patterns.push_back({
        std::regex(R"(\bnew\s+WebSocket\s*\()"),
        "WebSocket", UnsafeLevel::System
    });

    // Level 3: User input (browser-side)
    patterns.push_back({
        std::regex(R"(\b(prompt|confirm|alert)\s*\()"),
        "", UnsafeLevel::Input
    });

    return patterns;
}

std::string extractPatternName(const std::smatch& match, const std::string& fixedName) {
    if (!fixedName.empty()) return fixedName;
    std::string text = match.str();
    while (!text.empty() && (text.back() == '(' || text.back() == ' '))
        text.pop_back();
    return text;
}

/// Scope-opening regexes. Each captures the function/class name in group 1.
struct ScopePattern {
    std::regex regex;
    bool isClass;
    bool isFunction;
};

std::vector<ScopePattern> buildScopePatterns() {
    std::vector<ScopePattern> sp;
    // `function NAME(...)` or `export function NAME(...)`, including async.
    sp.push_back({std::regex(R"(^\s*(?:export\s+(?:default\s+)?)?(?:async\s+)?function\s*\*?\s*(\w+)\s*\()"),
                  /*isClass=*/false, /*isFunction=*/true});
    // `class NAME` or `export class NAME`.
    sp.push_back({std::regex(R"(^\s*(?:export\s+(?:default\s+)?)?(?:abstract\s+)?class\s+(\w+))"),
                  /*isClass=*/true, /*isFunction=*/false});
    // `const NAME = (args) => { ... }` / `let NAME = (args) => { ... }`
    sp.push_back({std::regex(R"(^\s*(?:export\s+)?(?:const|let|var)\s+(\w+)\s*=\s*(?:async\s+)?(?:\([^)]*\)|\w+)\s*=>\s*\{)"),
                  /*isClass=*/false, /*isFunction=*/true});
    // `const NAME = function(...) { ... }`
    sp.push_back({std::regex(R"(^\s*(?:export\s+)?(?:const|let|var)\s+(\w+)\s*=\s*(?:async\s+)?function\s*\*?\s*\w*\s*\()"),
                  /*isClass=*/false, /*isFunction=*/true});
    return sp;
}

} // anonymous namespace

std::vector<DetectedCallSite> TypeScriptCallSiteExtractor::extractCallSites(const std::string& filePath) {
    std::vector<DetectedCallSite> results;
    std::ifstream file(filePath);
    if (!file.is_open()) return results;

    static const auto patterns = buildPatterns();
    static const auto scopePatterns = buildScopePatterns();
    // Method shorthand inside class body: `NAME(args) { ... }` with optional
    // visibility modifier and `static` / `async`. Tolerates an optional
    // `: ReturnType` annotation between `)` and `{` — TypeScript class methods
    // routinely carry one. Also recognizes getter/setter (`get x()` /
    // `set x(v)`), generator (`*gen()`), `#private` names, and computed
    // (`[Symbol.iterator]()`) members so their bodies open a function scope
    // (otherwise dangerous calls inside them are misattributed to the class).
    static const std::regex methodRegex(
        R"(^\s*(?:(?:public|private|protected)\s+)?(?:static\s+)?(?:readonly\s+)?(?:async\s+)?(?:(?:get|set)\s+)?\*?\s*(#?\w+|\[[^\]]+\])\s*\([^)]*\)\s*(?::\s*[^{]+?)?\{)");
    // Class-field arrow method: `name = (args) => { ... }`,
    // `name = async (args) => { ... }`, or `name = arg => { ... }`, with an
    // optional `: Type` annotation on the field (which may itself be a
    // function type containing `=>`). Anchored on the trailing `=> {` so the
    // annotation's inner `=>` doesn't break the match. Its body is a function
    // scope (so calls inside it aren't misattributed to the class).
    static const std::regex arrowMethodRegex(
        R"(^\s*(?:(?:public|private|protected)\s+)?(?:static\s+)?(?:readonly\s+)?(#?\w+)\s*(?::\s*.+)?=\s*(?:async\s+)?(?:\([^)]*\)|\w+)\s*(?::\s*[^=]+?)?=>\s*\{\s*$)");

    std::vector<ScopeEntry> scopeStack;
    int braceDepth = 0;
    bool inBlockComment = false;
    std::string line;
    int lineNum = 0;

    while (std::getline(file, line)) {
        ++lineNum;

        // stripBlockCommentState retains code before a mid-line `/*`; process
        // it rather than dropping the whole line when block state stays open.
        std::string processed = stripBlockCommentState(line, inBlockComment);
        processed = stripLineComment(processed);
        if (processed.find_first_not_of(" \t\r\n") == std::string::npos) continue;

        int lineStartDepth = braceDepth;

        // Try scope-opening patterns first. They open a new scope at the
        // current brace depth, and the `{` on this line pushes depth by one.
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
        // Method shorthand only inside class body.
        if (!openedScope && !scopeStack.empty() && scopeStack.back().isClass) {
            std::smatch m;
            bool matched = std::regex_search(processed, m, methodRegex) ||
                           std::regex_search(processed, m, arrowMethodRegex);
            if (matched) {
                std::string name = m[1].str();
                // Skip reserved keywords / control flow tokens that match.
                static const std::vector<std::string> kws = {
                    "if", "for", "while", "switch", "return", "catch",
                    "do", "try", "else"};
                bool skip = name.empty();
                for (const auto& k : kws) if (name == k) { skip = true; break; }
                if (!skip) {
                    scopeStack.push_back({name, lineStartDepth,
                                          /*isClass=*/false, /*isFunction=*/true});
                    openedScope = true;
                }
            }
        }

        // Scan this line for dangerous patterns. The def line itself may
        // contain a dangerous default arg; we still scan.
        for (const auto& pat : patterns) {
            std::smatch match;
            std::string searchStr = processed;
            while (std::regex_search(searchStr, match, pat.regex)) {
                std::string patternName = extractPatternName(match, pat.patternName);

                // Skip `import(` inside an `import(...)` that is actually
                // `import type` / `import {` — these are static forms and
                // already matched by the Import extractor. The dynamic form
                // `import(expr)` takes a parenthesized expression.
                // Conservative: always emit; false positives are acceptable.
                (void)0;

                auto capability = classifyApiCall(patternName);

                DetectedCallSite site;
                site.calleePattern = patternName;
                site.callerQualifiedName = buildCallerName(scopeStack);
                site.capability = capability;
                site.unsafeLevel = pat.level;
                site.file = filePath;
                site.line = lineNum;
                results.push_back(std::move(site));

                searchStr = match.suffix().str();
            }
        }

        // Update brace depth.
        auto [opens, closes] = countBraces(processed);
        braceDepth += opens;
        braceDepth -= closes;
        if (braceDepth < 0) braceDepth = 0;

        // Pop scopes whose braceDepth is now past the boundary.
        while (!scopeStack.empty() && braceDepth <= scopeStack.back().braceDepth) {
            scopeStack.pop_back();
        }
    }

    return results;
}

} // namespace topo::check
