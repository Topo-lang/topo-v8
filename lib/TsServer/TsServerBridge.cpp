// TsServerBridge -- typescript-language-server LSP integration.
//
// Wraps `typescript-language-server --stdio` and implements the standard
// LSP query methods by delegating to the LSPBridge base class helpers.
// Mirrors PyrightBridge line-for-line; differences are limited to the
// executable name, the source-file suffixes scanned by the type-definition
// fallback, and the hover content shape (tsserver often returns an array
// of MarkedString entries in addition to the newer string / MarkupContent
// variants).

#include "TsServerBridge.h"

#include "topo/Platform/Platform.h"
#include "topo/Platform/Process.h"

#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace topo::lsp {

TsServerBridge::TsServerBridge() : LSPBridge("[tsserver] ") {}

bool TsServerBridge::isTsServerAvailable() {
    namespace plat = topo::platform;
    std::string exe = "typescript-language-server" + std::string(plat::ExeSuffix);
    // Shell-probe hardening:
    // the previous probe composed a shell command (``"exe" --version > /dev/null
    // 2>&1``) and called ``std::system``. That spawned a shell with quoted-
    // path semantics, duplicated POSIX/Windows redirection syntax across an
    // ``#ifdef _WIN32``, and was "one edit away from injection" if anyone
    // ever interpolated a caller-supplied path into ``exe``. Routing through
    // ``topo::platform::runProcessCapture`` uses argv-array exec, eliminates
    // the shell surface, and captures stdout/stderr without per-platform
    // redirection plumbing. Non-zero exit (or fail to spawn) → unavailable.
    auto result = plat::runProcessCapture(exe, {"--version"},
                                          /*verbose=*/false);
    return result.exitCode == 0;
}

bool TsServerBridge::start(const std::string& rootUri) {
    return start(std::string{}, rootUri);
}

bool TsServerBridge::start(const std::string& tsServerPath, const std::string& rootUri) {
    namespace plat = topo::platform;

    std::string exe = tsServerPath;
    if (exe.empty()) {
        // Default to the official tsserver LSP wrapper on $PATH. npm global
        // installs drop `typescript-language-server` onto PATH directly; we
        // do not hardcode any npm prefix.
        exe = "typescript-language-server" + std::string(plat::ExeSuffix);
    }

    std::vector<std::string> args = {"--stdio"};
    if (!startProcess(exe, args, rootUri))
        return false;

    parseSemanticTokenLegend();
    return true;
}

std::optional<SymbolResult> TsServerBridge::findDefinition(
    const std::string& qualifiedName,
    const std::vector<std::string>& /*tsFiles*/) {
    if (!isAvailable()) return std::nullopt;
    return queryWorkspaceSymbol(qualifiedName);
}

std::vector<SymbolResult> TsServerBridge::findReferences(
    const std::string& qualifiedName,
    const std::vector<std::string>& /*tsFiles*/) {
    if (!isAvailable()) return {};

    auto defn = queryWorkspaceSymbol(qualifiedName);
    if (!defn) return {};

    json params = {{"textDocument", {{"uri", pathToUri(defn->file)}}},
                   {"position", {{"line", defn->line}, {"character", defn->column}}},
                   {"context", {{"includeDeclaration", true}}}};

    auto response = sendRequest("textDocument/references", params);
    if (!response || !response->is_array()) return {};

    std::vector<SymbolResult> results;
    for (const auto& loc : *response) {
        SymbolResult r;
        r.file = uriToPath(loc["uri"].get<std::string>());
        r.line = loc["range"]["start"]["line"].get<int>();
        r.column = loc["range"]["start"]["character"].get<int>();
        results.push_back(std::move(r));
    }
    return results;
}

std::optional<std::string> TsServerBridge::getHoverInfo(
    const std::string& qualifiedName,
    const std::vector<std::string>& /*tsFiles*/) {
    if (!isAvailable()) return std::nullopt;

    auto defn = queryWorkspaceSymbol(qualifiedName);
    if (!defn) return std::nullopt;

    json params = {{"textDocument", {{"uri", pathToUri(defn->file)}}},
                   {"position", {{"line", defn->line}, {"character", defn->column}}}};

    auto response = sendRequest("textDocument/hover", params);
    if (!response || response->is_null()) return std::nullopt;

    if (!response->contains("contents")) return std::nullopt;
    return extractHoverString((*response)["contents"]);
}

std::optional<std::string> TsServerBridge::extractHoverString(const json& contents) {
    // LSP 3.x hover `contents` may be:
    //   1. `string`                             — legacy plain text
    //   2. `MarkupContent`   { kind, value }    — modern markdown/plain
    //   3. `MarkedString`    string | { language, value }
    //   4. `MarkedString[]`  — typescript-language-server's common shape
    auto extractMarkedString = [](const json& node) -> std::optional<std::string> {
        if (node.is_string()) {
            return node.get<std::string>();
        }
        if (node.is_object() && node.contains("value")) {
            return node["value"].get<std::string>();
        }
        return std::nullopt;
    };

    if (contents.is_string()) {
        return contents.get<std::string>();
    }
    if (contents.is_object()) {
        if (auto v = extractMarkedString(contents)) return v;
        return std::nullopt;
    }
    if (contents.is_array()) {
        std::string joined;
        for (const auto& entry : contents) {
            if (auto v = extractMarkedString(entry)) {
                if (!joined.empty()) joined += "\n\n";
                joined += *v;
            }
        }
        if (!joined.empty()) return joined;
    }
    return std::nullopt;
}

std::optional<SymbolResult> TsServerBridge::findTypeDefinition(
    const std::string& typeName,
    const std::vector<std::string>& sourceFiles,
    const std::vector<std::string>& /*includeDirs*/) {
    // Prefer the live index when tsserver is running.
    if (isAvailable()) {
        auto result = queryWorkspaceSymbol(typeName);
        if (result) return result;
    }

    // Fallback: scan .ts / .tsx source files for a matching class / interface /
    // type alias definition.  Very forgiving regex -- false positives are fine,
    // false negatives would silently lose type-definition targets.
    const std::regex pattern(
        R"(^\s*(?:export\s+)?(?:abstract\s+)?(?:class|interface|type)\s+)" +
        typeName + R"((?:[\s<({=;:]|$))");

    for (const auto& filePath : sourceFiles) {
        const auto suffixIs = [&](const std::string& s) {
            return filePath.size() >= s.size() &&
                   filePath.substr(filePath.size() - s.size()) == s;
        };
        if (!suffixIs(".ts") && !suffixIs(".tsx")) continue;

        std::ifstream file(filePath);
        if (!file.is_open()) continue;

        std::string line;
        int lineNo = 0;
        while (std::getline(file, line)) {
            ++lineNo;
            if (std::regex_search(line, pattern)) {
                return SymbolResult{filePath, lineNo, 0};
            }
        }
    }

    return std::nullopt;
}

} // namespace topo::lsp
