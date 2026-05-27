// V8SourceScanner — shared scanning helpers for the V8-family regex extractors.
//
// Five regex-based L1 extractors under topo-v8/lib/Check/Extract/ (Symbol,
// Import, CallSite, CallEdge, SymbolAccess) historically each carried their
// own copy of stripLineComment / stripBlockCommentState / countBraces /
// maskStringLiterals / ScopeEntry. When a new TypeScript syntax form
// landed, every extractor had to be updated in lock-step — missed updates
// produce false negatives, which the extractor file headers explicitly
// call out as safety issues.
//
// This header consolidates the shared scaffolding into one internal
// header consumed by every extractor's `.cpp`. The helpers stay in an
// inline-namespace-free anonymous-equivalent (the file is private to
// topo-v8/lib/Check/Extract/ and the symbols are header-defined
// `inline` so the linker does not see duplicates across translation
// units). Each extractor keeps its own per-extractor regex patterns;
// only the genuinely shared scanning primitives live here.
//
// Issue: typescript-extractors-duplicate-scope-tracking-helpers
//   — pins the rationale and the call sites.

#ifndef TOPO_V8_LIB_CHECK_EXTRACT_V8_SOURCE_SCANNER_H
#define TOPO_V8_LIB_CHECK_EXTRACT_V8_SOURCE_SCANNER_H

#include <string>
#include <utility>
#include <vector>

namespace topo::check::v8scanner {

/// Brace-scope entry tracked by extractors during their per-file walk.
/// The shape is a small superset that all five extractors can use —
/// `isClass`, `isFunction`, and `isNamespace` cover every per-extractor
/// distinction without forcing the others to opt in to a separate type.
/// Extractors that don't care about a field simply leave it false.
struct ScopeEntry {
    std::string name;
    int braceDepth = 0;  // depth at which this scope opened
    bool isClass = false;
    bool isFunction = false;
    bool isNamespace = false;
};

/// Strip the trailing `//` line comment from `line`, respecting (very
/// approximately) string / template-literal boundaries: any `//` outside
/// an odd number of unescaped quote characters is treated as a comment
/// start. Backslash-escaped characters are skipped.
inline std::string stripLineComment(const std::string& line) {
    bool inSingle = false;
    bool inDouble = false;
    bool inBacktick = false;
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        char c = line[i];
        if (c == '\\') { ++i; continue; }  // skip escaped char
        if (!inDouble && !inBacktick && c == '\'') inSingle = !inSingle;
        else if (!inSingle && !inBacktick && c == '"') inDouble = !inDouble;
        else if (!inSingle && !inDouble && c == '`') inBacktick = !inBacktick;
        else if (!inSingle && !inDouble && !inBacktick &&
                 c == '/' && line[i + 1] == '/') {
            return line.substr(0, i);
        }
    }
    return line;
}

/// Strip `/* ... */` block-comment spans, threading the in-block state
/// across lines. `inBlock` is updated on entry/exit; the returned string
/// has the comment characters removed (other text preserved).
inline std::string stripBlockCommentState(const std::string& line, bool& inBlock) {
    std::string out;
    std::size_t i = 0;
    while (i < line.size()) {
        if (inBlock) {
            auto end = line.find("*/", i);
            if (end == std::string::npos) return out;
            inBlock = false;
            i = end + 2;
        } else {
            if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
                inBlock = true;
                i += 2;
            } else {
                out.push_back(line[i]);
                ++i;
            }
        }
    }
    return out;
}

/// Mask string and template-literal contents with spaces so that
/// call-like tokens inside strings don't get matched by downstream
/// regexes. Single-line template-literal spans are masked; multi-line
/// template literals are out of scope (the extractors call out the gap).
inline std::string maskStringLiterals(const std::string& line) {
    std::string out = line;
    for (std::size_t i = 0; i < out.size(); ++i) {
        char c = out[i];
        if (c == '"' || c == '\'' || c == '`') {
            char quote = c;
            out[i] = ' ';
            ++i;
            while (i < out.size() && out[i] != quote) {
                if (out[i] == '\\' && i + 1 < out.size()) {
                    out[i] = ' ';
                    out[i + 1] = ' ';
                    i += 2;
                    continue;
                }
                out[i] = ' ';
                ++i;
            }
            if (i < out.size()) out[i] = ' ';
        }
    }
    return out;
}

/// Count `{` / `}` characters in `line`, ignoring those inside string
/// or template-literal spans. Returns `(opens, closes)`.
inline std::pair<int, int> countBraces(const std::string& line) {
    int opens = 0, closes = 0;
    bool inSingle = false, inDouble = false, inBacktick = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\') { ++i; continue; }
        if (!inDouble && !inBacktick && c == '\'') { inSingle = !inSingle; continue; }
        if (!inSingle && !inBacktick && c == '"') { inDouble = !inDouble; continue; }
        if (!inSingle && !inDouble && c == '`') { inBacktick = !inBacktick; continue; }
        if (inSingle || inDouble || inBacktick) continue;
        if (c == '{') ++opens;
        else if (c == '}') ++closes;
    }
    return {opens, closes};
}

} // namespace topo::check::v8scanner

#endif // TOPO_V8_LIB_CHECK_EXTRACT_V8_SOURCE_SCANNER_H
