#ifndef TOPO_V8_TSSERVER_TSSERVERBRIDGE_H
#define TOPO_V8_TSSERVER_TSSERVERBRIDGE_H

#include "topo/LSP/LSPBridge.h"

namespace topo::lsp {

class TsServerBridge : public LSPBridge {
public:
    TsServerBridge();
    bool start(const std::string& rootUri) override;
    std::string displayName() const override { return "TypeScript"; }

    /// Whether `typescript-language-server` is resolvable and runnable on the
    /// host (probed via `--version`). Mirrors ClangdBridge::isClangdAvailable;
    /// used by L2 tests to skip cleanly when tsserver is not installed.
    static bool isTsServerAvailable();

    bool start(const std::string& tsServerPath, const std::string& rootUri);
    std::optional<SymbolResult> findDefinition(const std::string& qualifiedName,
                                               const std::vector<std::string>& tsFiles) override;
    std::vector<SymbolResult> findReferences(const std::string& qualifiedName,
                                             const std::vector<std::string>& tsFiles) override;
    std::optional<std::string> getHoverInfo(const std::string& qualifiedName,
                                            const std::vector<std::string>& tsFiles) override;

    /// Find host-language type definition for a named type.
    /// Queries tsserver workspace index first; falls back to scanning sourceFiles
    /// (.ts / .tsx) for class/interface/type definitions matching typeName.
    std::optional<SymbolResult> findTypeDefinition(const std::string& typeName,
                                                   const std::vector<std::string>& sourceFiles,
                                                   const std::vector<std::string>& includeDirs) override;

    std::string languageId() const override { return "typescript"; }

    /// Extract a human-readable string from the LSP `hover.contents` value.
    /// Handles the four documented LSP 3.x shapes:
    ///   1. `string`                                — legacy plain text
    ///   2. `MarkupContent`   { kind, value }       — modern markdown/plain
    ///   3. `MarkedString`    string | { language, value }
    ///   4. `MarkedString[]`  — typescript-language-server's common shape
    ///
    /// Exposed as a static helper so unit tests can exercise each shape
    /// without spawning a real tsserver subprocess — otherwise this
    /// parsing logic would have only integration coverage. Returns
    /// nullopt when the node shape is none of the above or the array
    /// is empty.
    static std::optional<std::string> extractHoverString(const json& contents);
};

} // namespace topo::lsp
#endif
