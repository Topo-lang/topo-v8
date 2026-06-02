// SourceMapResolver unit coverage.
//
// Fixtures are written to a per-test temp directory and torn down on
// completion. The maps embedded inline are minimal v3 documents emitted
// to exercise the VLQ decoder, segment-lookup, missing-file fallback,
// and malformed-input behavior. We do NOT depend on tsc at test time.

#include "SourceMapResolver.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace fs = std::filesystem;

namespace {

// Portable environment mutation — Windows has no POSIX setenv/unsetenv.
void setEnvVar(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    ::setenv(name, value, /*overwrite=*/1);
#endif
}

void unsetEnvVar(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

fs::path makeTempDir(const std::string& tag) {
    fs::path base = fs::temp_directory_path() /
                    ("topo-v8-sourcemap-" + tag + "-" +
                     std::to_string(::testing::UnitTest::GetInstance()
                                        ->random_seed()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

void writeFile(const fs::path& p, const std::string& body) {
    std::ofstream out(p);
    out << body;
}

// Hand-crafted source map describing two generated lines:
//   line 1 (genLine 0): single segment at column 0 → sources[0] line 0 col 0
//   line 2 (genLine 1): single segment at column 2 → sources[0] line 4 col 1
//                       (delta encoded relative to the previous segment)
//
// VLQ-encoded:
//   first segment "AAAA": gen0,src0,line0,col0
//   `;` separator
//   second segment "EAIC": gen+2, src+0, line+4, col+1
//                          (E=2, A=0, I=8→4 signed, C=2→1 signed)
//
// The 5-field name index is omitted here.
constexpr const char* kBasicMap = R"({
  "version": 3,
  "sources": ["index.ts"],
  "names": [],
  "mappings": "AAAA;EAIC"
})";

} // namespace

TEST(SourceMapResolverTest, BasicLookupResolvesJsToTs) {
    fs::path tmp = makeTempDir("basic");
    fs::path js = tmp / "index.js";
    fs::path map = tmp / "index.js.map";
    writeFile(js, "// generated\n// generated\n");
    writeFile(map, kBasicMap);

    topo::v8::debug::SourceMapResolver r;
    auto a = r.resolve(js.string(), 1, 1);
    ASSERT_TRUE(a.has_value()) << "first-line lookup should resolve";
    EXPECT_EQ(a->line_1indexed, 1);
    EXPECT_EQ(a->column_1indexed, 1);
    EXPECT_NE(a->source_path.find("index.ts"), std::string::npos);

    auto b = r.resolve(js.string(), 2, 3);
    ASSERT_TRUE(b.has_value()) << "second-line lookup should resolve";
    EXPECT_EQ(b->line_1indexed, 5);   // 4 (0-indexed in map) + 1
    EXPECT_EQ(b->column_1indexed, 2); // 1 (0-indexed in map) + 1
}

TEST(SourceMapResolverTest, MissingMapReturnsNullopt) {
    fs::path tmp = makeTempDir("missing");
    fs::path js = tmp / "only.js";
    writeFile(js, "// generated\n");
    // No .map file written.

    topo::v8::debug::SourceMapResolver r;
    auto x = r.resolve(js.string(), 1, 1);
    EXPECT_FALSE(x.has_value());
}

TEST(SourceMapResolverTest, MalformedMapReturnsNullopt) {
    fs::path tmp = makeTempDir("malformed");
    fs::path js = tmp / "broken.js";
    fs::path map = tmp / "broken.js.map";
    writeFile(js, "// generated\n");
    // Non-JSON garbage.
    writeFile(map, "this is not a source map }}}");

    topo::v8::debug::SourceMapResolver r;
    auto x = r.resolve(js.string(), 1, 1);
    EXPECT_FALSE(x.has_value());

    // Wrong version → also nullopt, cached.
    fs::path js2 = tmp / "v2.js";
    fs::path map2 = tmp / "v2.js.map";
    writeFile(js2, "// generated\n");
    writeFile(map2,
              R"({"version": 2, "sources": ["x.ts"], "mappings": "AAAA"})");
    auto y = r.resolve(js2.string(), 1, 1);
    EXPECT_FALSE(y.has_value());
}

TEST(SourceMapResolverTest, SearchRootIsConsulted) {
    fs::path tmp = makeTempDir("searchroot");
    fs::path js = tmp / "no-sibling.js";
    writeFile(js, "// generated\n");
    // Sibling .map intentionally absent; place it under a separate root.
    fs::path otherDir = tmp / "maps";
    fs::create_directories(otherDir);
    writeFile(otherDir / "no-sibling.js.map", kBasicMap);

    topo::v8::debug::SourceMapResolver r;
    r.addSearchRoot(otherDir.string());
    auto a = r.resolve(js.string(), 1, 1);
    ASSERT_TRUE(a.has_value());
    EXPECT_NE(a->source_path.find("index.ts"), std::string::npos);
}

TEST(SourceMapResolverTest, CacheReturnsConsistentResultAcrossCalls) {
    fs::path tmp = makeTempDir("cache");
    fs::path js = tmp / "cached.js";
    fs::path map = tmp / "cached.js.map";
    writeFile(js, "// generated\n");
    writeFile(map, kBasicMap);

    topo::v8::debug::SourceMapResolver r;
    auto a1 = r.resolve(js.string(), 1, 1);
    ASSERT_TRUE(a1.has_value());

    // Deleting the map after the first resolve must not change subsequent
    // results — the cached parse is reused.
    std::error_code ec;
    fs::remove(map, ec);
    auto a2 = r.resolve(js.string(), 1, 1);
    ASSERT_TRUE(a2.has_value());
    EXPECT_EQ(a1->source_path, a2->source_path);
    EXPECT_EQ(a1->line_1indexed, a2->line_1indexed);
}

// VLQ decoder coverage: roundtrip a few known segments via the public
// API. We construct maps that encode specific deltas and verify the
// recovered source coordinates match.
TEST(SourceMapResolverTest, VlqDecoderHandlesKnownSegments) {
    fs::path tmp = makeTempDir("vlq");
    fs::path js = tmp / "vlq.js";
    fs::path map = tmp / "vlq.js.map";
    writeFile(js, "// generated\n");

    // mappings = "AAAA,GAAC"
    //   segment 1: AAAA → gen0, src0, line0, col0
    //   segment 2: GAAC → +3 (G=3) gen → genCol 3, src+0, line+0, col+1 (C=2 signed)
    writeFile(map, R"({
        "version": 3,
        "sources": ["one.ts"],
        "names": [],
        "mappings": "AAAA,GAAC"
    })");

    topo::v8::debug::SourceMapResolver r;
    // Lookup column 1 hits segment 1.
    auto a = r.resolve(js.string(), 1, 1);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->line_1indexed, 1);
    EXPECT_EQ(a->column_1indexed, 1);

    // Lookup column 4 hits segment 2 (genCol 3 + 0-index = column 4 1-indexed).
    auto b = r.resolve(js.string(), 1, 4);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->line_1indexed, 1);
    EXPECT_EQ(b->column_1indexed, 2); // col 1 0-indexed + 1
}

// LRU eviction: when more than `TOPO_V8_SOURCEMAP_CACHE` distinct
// js_path entries are resolved, the oldest must be dropped from the
// cache. We verify by deleting a `.map` file from disk, then driving
// enough fresh resolves to push the deleted entry past the LRU tail.
// A subsequent resolve of the deleted js_path now misses on disk (no
// cached parse to fall back on) and returns nullopt.
TEST(SourceMapResolverTest, CacheEvictsLruEntriesPastCapacity) {
    fs::path tmp = makeTempDir("lru");
    fs::path victimJs = tmp / "victim.js";
    fs::path victimMap = tmp / "victim.js.map";
    writeFile(victimJs, "// generated\n");
    writeFile(victimMap, kBasicMap);

    // Tight cap so the test does not need to construct hundreds of
    // fixtures; matches what an LSP-server caller would do to bound
    // memory in a long-running session.
    setEnvVar("TOPO_V8_SOURCEMAP_CACHE", "4");
    topo::v8::debug::SourceMapResolver r;

    // Prime the victim entry.
    auto v0 = r.resolve(victimJs.string(), 1, 1);
    ASSERT_TRUE(v0.has_value());

    // Delete the on-disk map so any later resolve depends solely on
    // the cache.
    std::error_code ec;
    fs::remove(victimMap, ec);

    // Cache still has the victim — subsequent calls succeed.
    auto vCached = r.resolve(victimJs.string(), 1, 1);
    ASSERT_TRUE(vCached.has_value());

    // Push enough distinct fresh entries through to evict the victim
    // (cap is 4; 5 fresh paths after the victim → victim drops off).
    for (int i = 0; i < 5; ++i) {
        fs::path fjs = tmp / ("filler" + std::to_string(i) + ".js");
        fs::path fmap = tmp / ("filler" + std::to_string(i) + ".js.map");
        writeFile(fjs, "// generated\n");
        writeFile(fmap, kBasicMap);
        auto fr = r.resolve(fjs.string(), 1, 1);
        ASSERT_TRUE(fr.has_value()) << "filler " << i << " should parse";
    }

    // Victim should now be evicted — and because the on-disk map is
    // gone, a fresh resolve returns nullopt rather than re-using the
    // stale cached parse.
    auto vAfter = r.resolve(victimJs.string(), 1, 1);
    EXPECT_FALSE(vAfter.has_value())
        << "LRU should have evicted the victim entry past the cap";

    unsetEnvVar("TOPO_V8_SOURCEMAP_CACHE");
}
