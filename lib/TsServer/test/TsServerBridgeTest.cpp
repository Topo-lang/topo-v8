// Unit tests for TsServerBridge in-process slices.
//
// Two surfaces are exercised here without spawning a live
// `typescript-language-server`:
//
//   1. extractHoverString  — the LSP 3.x `hover.contents` shape parser
//      (string / MarkupContent / MarkedString / MarkedString[]).
//      Audited as the "intricate" branch likely to regress in
//      isolation; integration coverage requires tsserver to be running
//      *and* to return each shape, neither guaranteed.
//
//   2. findTypeDefinition  — the filesystem-fallback regex branch
//      (`.ts` / `.tsx` class / interface / type alias). Reachable when
//      isAvailable() returns false, i.e. the bridge was never started.
//
// These slices give in-process coverage to logic that the live-tsserver
// integration suite cannot reliably exercise on its own.

#include "TsServerBridge.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;
using topo::lsp::TsServerBridge;

// ── extractHoverString — LSP hover.contents shape parser ───────────

TEST(TsServerExtractHoverString, LegacyPlainString) {
    auto result = TsServerBridge::extractHoverString(json("plain hover"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "plain hover");
}

TEST(TsServerExtractHoverString, ModernMarkupContentObject) {
    json contents = {{"kind", "markdown"}, {"value", "**bold** text"}};
    auto result = TsServerBridge::extractHoverString(contents);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "**bold** text");
}

TEST(TsServerExtractHoverString, MarkedStringObjectWithLanguage) {
    json contents = {{"language", "typescript"}, {"value", "function foo(): void"}};
    auto result = TsServerBridge::extractHoverString(contents);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "function foo(): void");
}

TEST(TsServerExtractHoverString, MarkedStringArrayJoinsWithBlankLine) {
    json contents = json::array();
    contents.push_back("first segment");
    contents.push_back({{"language", "typescript"}, {"value", "second segment"}});
    auto result = TsServerBridge::extractHoverString(contents);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "first segment\n\nsecond segment");
}

TEST(TsServerExtractHoverString, MarkedStringArrayEmpty) {
    json contents = json::array();
    auto result = TsServerBridge::extractHoverString(contents);
    EXPECT_FALSE(result.has_value());
}

TEST(TsServerExtractHoverString, MarkedStringArrayAllEntriesUnparseable) {
    // Entries that are neither strings nor objects with `value` are
    // silently skipped per the existing logic. An array of only such
    // entries yields no joined text, hence nullopt — this pins that
    // contract.
    json contents = json::array();
    contents.push_back(42);
    contents.push_back(json::object());
    auto result = TsServerBridge::extractHoverString(contents);
    EXPECT_FALSE(result.has_value());
}

TEST(TsServerExtractHoverString, ObjectWithoutValueField) {
    // An object without a `value` field is not a MarkupContent /
    // MarkedString shape; extractor must not crash, must return nullopt.
    json contents = {{"kind", "markdown"}, {"unrelated", "ignored"}};
    auto result = TsServerBridge::extractHoverString(contents);
    EXPECT_FALSE(result.has_value());
}

TEST(TsServerExtractHoverString, NullContents) {
    auto result = TsServerBridge::extractHoverString(json());
    EXPECT_FALSE(result.has_value());
}

// ── findTypeDefinition — filesystem regex fallback path ────────────

namespace {

/// Write `content` to a unique file under `dir` with the given suffix,
/// returning the full path. The fixture writer leaves the file in
/// place; the test's TempDirFixture cleans up at TearDown.
std::string writeFixture(const fs::path& dir, const std::string& filename,
                         const std::string& content) {
    fs::path p = dir / filename;
    std::ofstream out(p);
    out << content;
    out.close();
    return p.string();
}

class TsServerFindTypeDefTest : public ::testing::Test {
protected:
    fs::path tmp;
    void SetUp() override {
        // unique per-test temp dir
        std::string tpl = (fs::temp_directory_path() / "topo-tsserver-test-XXXXXX").string();
        std::vector<char> buf(tpl.begin(), tpl.end());
        buf.push_back('\0');
        char* p = mkdtemp(buf.data());
        ASSERT_NE(p, nullptr);
        tmp = fs::path(p);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }
};

} // namespace

TEST_F(TsServerFindTypeDefTest, RegexFindsClassInTsFile) {
    // A fresh TsServerBridge has not called start(), so isAvailable() is
    // false and findTypeDefinition takes the filesystem-fallback path.
    TsServerBridge bridge;
    std::string f = writeFixture(tmp, "thing.ts",
        "// header\nexport class Widget {\n  doIt(): void {}\n}\n");

    auto result = bridge.findTypeDefinition("Widget", {f}, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->file, f);
    EXPECT_EQ(result->line, 2);  // class on line 2 (1-based)
}

TEST_F(TsServerFindTypeDefTest, RegexFindsInterface) {
    TsServerBridge bridge;
    std::string f = writeFixture(tmp, "iface.ts",
        "export interface Movable {\n  move(): void;\n}\n");

    auto result = bridge.findTypeDefinition("Movable", {f}, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->file, f);
    EXPECT_EQ(result->line, 1);
}

TEST_F(TsServerFindTypeDefTest, RegexFindsTypeAlias) {
    TsServerBridge bridge;
    std::string f = writeFixture(tmp, "alias.ts",
        "export type Id = number;\n");

    auto result = bridge.findTypeDefinition("Id", {f}, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->line, 1);
}

TEST_F(TsServerFindTypeDefTest, RegexFindsAbstractClass) {
    TsServerBridge bridge;
    std::string f = writeFixture(tmp, "abst.ts",
        "export abstract class Base {\n}\n");

    auto result = bridge.findTypeDefinition("Base", {f}, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->line, 1);
}

TEST_F(TsServerFindTypeDefTest, RegexFindsClassInTsxFile) {
    // .tsx is also accepted by the suffix check.
    TsServerBridge bridge;
    std::string f = writeFixture(tmp, "comp.tsx",
        "export class App {\n  render() {}\n}\n");

    auto result = bridge.findTypeDefinition("App", {f}, {});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->line, 1);
}

TEST_F(TsServerFindTypeDefTest, SkipsNonTsExtensions) {
    // The fallback is documented to scan .ts/.tsx only. A .js file
    // containing the type name MUST be ignored.
    TsServerBridge bridge;
    std::string f = writeFixture(tmp, "stuff.js",
        "export class Widget {}\n");

    auto result = bridge.findTypeDefinition("Widget", {f}, {});
    EXPECT_FALSE(result.has_value());
}

TEST_F(TsServerFindTypeDefTest, MissingTypeReturnsNullopt) {
    TsServerBridge bridge;
    std::string f = writeFixture(tmp, "thing.ts",
        "export class Other {}\n");

    auto result = bridge.findTypeDefinition("NotPresent", {f}, {});
    EXPECT_FALSE(result.has_value());
}
