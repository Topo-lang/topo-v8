// Live round-trip tests for TsServerBridge — exercises a real
// `typescript-language-server` spawned by bare name on PATH (on Windows
// that resolves the npm `.cmd` shim through the platform spawn layer,
// the exact shape unit tests cannot cover).
//
// Each case GTEST_SKIP()s only when isTsServerAvailable() is false
// (server not installed). Once the server is present, protocol
// misbehavior FAILS — never skips. CI installs the server on every
// matrix leg and a workflow guard converts a skip into a hard failure.

#include "TsServerBridge.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using topo::lsp::SymbolResult;
using topo::lsp::TsServerBridge;

namespace {

// Poll-until-ready helper: tsserver's workspace index can lag behind
// didOpen on a cold start. Retrying until a bounded deadline keeps the
// happy-path assertions running instead of failing on index latency.
// The missing-server case is gated by isTsServerAvailable() in SetUp,
// so a persistent miss here is a genuine protocol failure.
template <typename T>
std::optional<T> pollUntil(std::function<std::optional<T>()> query,
                           std::chrono::milliseconds budget = std::chrono::milliseconds{15000},
                           std::chrono::milliseconds interval = std::chrono::milliseconds{500}) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        auto r = query();
        if (r.has_value()) return r;
        if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        std::this_thread::sleep_for(interval);
    }
}

// file:// URI for the workspace root. POSIX absolute paths already start
// with '/'; Windows drive-letter paths (C:/...) need the extra slash or
// the drive would parse as the URI authority.
std::string rootUriFor(const fs::path& dir) {
    std::string s = dir.generic_string();
    if (!s.empty() && s[0] == '/') return "file://" + s;
    return "file:///" + s;
}

class TsServerBridgeLive : public ::testing::Test {
protected:
    fs::path tmp;
    std::string widgetFile;
    TsServerBridge bridge;

    void SetUp() override {
        if (!TsServerBridge::isTsServerAvailable()) {
            GTEST_SKIP() << "typescript-language-server not found on PATH";
        }

        // Unique per-test temp workspace. Portable (Windows has no
        // mkdtemp): pick a fresh name under the temp root and create it,
        // retrying on collision.
        std::error_code ec;
        static int counter = 0;
        const int seed = ::testing::UnitTest::GetInstance()->random_seed();
        for (int attempt = 0; attempt < 256; ++attempt) {
            fs::path candidate = fs::temp_directory_path() /
                ("topo-tsserver-live-" + std::to_string(seed) + "-" +
                 std::to_string(++counter));
            if (fs::create_directory(candidate, ec) && !ec) {
                tmp = candidate;
                break;
            }
        }
        ASSERT_FALSE(tmp.empty()) << "failed to create a unique temp dir";

        // Pinned fixture: the assertions below depend on these exact
        // 0-based coordinates — edit both together.
        //   line 1, col 6  -> inside `compute`
        //   line 3, col 50 -> the `Widget` in `new Widget()`
        writeFile("widget.ts",
                  "export class Widget {\n"
                  "    compute(x: number): number { return x * 2; }\n"
                  "}\n"
                  "export function makeWidget(): Widget { return new Widget(); }\n");
        writeFile("tsconfig.json", "{\"compilerOptions\":{\"strict\":true}}\n");
        widgetFile = (tmp / "widget.ts").string();

        // Empty path = bare-name PATH resolution — on Windows this is the
        // npm `.cmd` shim route this suite exists to exercise. The server
        // is known installed here, so a failed handshake FAILS.
        ASSERT_TRUE(bridge.start(std::string{}, rootUriFor(tmp)))
            << "typescript-language-server is on PATH but failed the "
               "initialize handshake";
        bridge.setTimeouts(std::chrono::milliseconds{30000},
                           std::chrono::milliseconds{10000});
        // tsserver only indexes files it has been told about; didOpen also
        // triggers project load for the workspace.
        bridge.openDocument(widgetFile);
        (void)bridge.waitForIndex(std::chrono::milliseconds{15000});
    }

    void TearDown() override {
        bridge.stop();  // idempotent; safe after StartInitializeStop's stop()
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }

    void writeFile(const std::string& name, const std::string& content) {
        std::ofstream out(tmp / name);
        out << content;
    }
};

} // namespace

TEST_F(TsServerBridgeLive, StartInitializeStop) {
    // SetUp drove the full start -> initialize -> initialized handshake.
    EXPECT_TRUE(bridge.isAvailable());
    bridge.stop();
    EXPECT_FALSE(bridge.isAvailable());
}

TEST_F(TsServerBridgeLive, HoverRoundTrip) {
    // (1,6) is inside `compute`; tsserver answers with a signature shaped
    // like "(method) Widget.compute(x: number): number".
    auto hover = pollUntil<std::string>(
        [&] { return bridge.getHoverAt(widgetFile, 1, 6); });
    ASSERT_TRUE(hover.has_value());
    EXPECT_NE(hover->find("compute"), std::string::npos) << *hover;
}

TEST_F(TsServerBridgeLive, DefinitionRoundTrip) {
    // (3,50) is the `Widget` in `new Widget()`; its definition is the
    // class declaration on line 0 (raw 0-based LSP values).
    auto defn = pollUntil<SymbolResult>(
        [&] { return bridge.getDefinitionAt(widgetFile, 3, 50); });
    ASSERT_TRUE(defn.has_value());
    EXPECT_NE(defn->file.find("widget.ts"), std::string::npos) << defn->file;
    EXPECT_EQ(defn->line, 0);
}

TEST_F(TsServerBridgeLive, FindDefinitionViaWorkspaceSymbol) {
    // findDefinition routes through workspace/symbol. The index can lag a
    // didOpen on cold start, so poll — but a miss after the full budget
    // FAILS (the server is known installed here), never skips.
    auto defn = pollUntil<SymbolResult>(
        [&] { return bridge.findDefinition("Widget", {widgetFile}); });
    ASSERT_TRUE(defn.has_value());
    EXPECT_NE(defn->file.find("widget.ts"), std::string::npos) << defn->file;
}
