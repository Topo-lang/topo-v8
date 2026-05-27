#ifndef TOPO_V8_DEBUG_SOURCEMAPRESOLVER_H
#define TOPO_V8_DEBUG_SOURCEMAPRESOLVER_H

// Source Map v3 resolver for V8 family hosts.
//
// V8 reports stack frames in terms of the executed `.js` file. When the
// `.js` was produced by tsc (or esbuild / swc) with `--sourceMap`, a
// sibling `.js.map` records the (generatedLine, generatedColumn) →
// (sourceFile, sourceLine, sourceColumn, name?) mapping needed to recover
// the original `.ts` (or `.tsx`) source location. This resolver loads
// those maps lazily and answers point queries.
//
// Wire format reference:
//   https://sourcemaps.info/spec.html
//   §4.1 — JSON envelope (`version: 3`, `sources: [...]`, `names: [...]`,
//          `mappings: "..."`, optional `sourceRoot`)
//   §4.1 mappings — VLQ-base64 encoded segments, `,` between segments on
//          the same line, `;` between lines; absolute on first segment of
//          a line for the generated column, all four other fields are
//          deltas from the previous segment of the same map (sourceIndex,
//          sourceLine, sourceColumn, nameIndex).
//
// Lookup strategy when at least one search root has been registered:
//   1.  `<js_path>.map`                            (sibling `.map`)
//   2.  `<js_path stripped of .js>.js.map`         (always equals 1 for
//                                                   `.js` inputs but
//                                                   handles `.mjs` /
//                                                   `.cjs` callers)
//   3.  for each registered search root R:
//          `R / basename(js_path) + ".map"`
//
// Missing or malformed map → `resolve` returns `std::nullopt`. The
// resolver never throws; callers are expected to degrade gracefully (the
// CpuProfileConverter falls back to reporting the original `.js` frame).
//
// Per-`js_path` results are cached so subsequent samples touching the
// same file pay the parse cost once. Cache stores the parsed segment
// table indexed by generated line; lookups bisect within a line.
//
// The cache is LRU-bounded: each lookup moves the touched entry to
// the front, and once the entry count exceeds the capacity the
// least-recently-used entry is evicted. Default capacity is 256
// entries (a typical webpack `.js.map` can be several MB; the
// default keeps the working set in the hundreds-of-MB range without
// unbounded growth in a long-lived host such as an LSP server).
// Override via the `TOPO_V8_SOURCEMAP_CACHE` env var (positive
// integer; zero or negative falls back to the default).

#include "topo/Profile/FrameResolver.h"

#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace topo::v8::debug {

// Thin alias of the host-agnostic ResolvedFrame surface so V8-side
// callers can name the type with a V8-local identifier. Field semantics
// (1-indexed line/column, raw source path, optional name) are owned by
// the topo-core declaration in `topo/Profile/FrameResolver.h`.
using ResolvedFrame = topo::profile::ResolvedFrame;

class SourceMapResolver final : public topo::profile::FrameResolver {
public:
    SourceMapResolver();
    ~SourceMapResolver() override;

    SourceMapResolver(const SourceMapResolver&) = delete;
    SourceMapResolver& operator=(const SourceMapResolver&) = delete;

    // Register a directory to search when neither sibling nor stem-based
    // probes find a `.map`. Multiple roots are searched in registration
    // order. Non-existent paths are silently kept — the lookup itself
    // tolerates missing files.
    void addSearchRoot(std::string dir);

    // Look up the (line_1indexed, column_1indexed) position in
    // `js_path`. Lines and columns are 1-indexed on the boundary (V8
    // wire is 0-indexed; the converter that calls us has already added
    // the +1). Returns nullopt when:
    //   - no `.map` is found via the strategy above
    //   - the `.map` is unreadable or fails JSON / version-3 validation
    //   - the requested generated position is below the first mapping
    //     for its line (V8 sometimes reports column 0 on a line whose
    //     first real mapping starts at a later column — we return the
    //     entry covering the position, choosing the rightmost segment
    //     with generatedColumn <= column).
    std::optional<ResolvedFrame> resolve(const std::string& js_path,
                                         int line_1indexed,
                                         int column_1indexed) override;

    // Exposed publicly so the .cpp's helper functions (kept in an
    // anonymous namespace for translation-unit privacy) can reference
    // these as `SourceMapResolver::Segment` / `ParsedMap`. End-users
    // should treat them as implementation detail.
    struct Segment {
        int generatedColumn = 0;
        int sourceIndex = 0;     // index into `sources_`
        int sourceLine = 0;      // 0-indexed in the original spec
        int sourceColumn = 0;    // 0-indexed in the original spec
        int nameIndex = -1;      // index into `names_` or -1
    };

    struct ParsedMap {
        // Per-line segment table. `lines_[L][k]` is the k-th segment on
        // generated line L (0-indexed). Each line's segments are sorted
        // by generatedColumn ascending (the spec guarantees this).
        std::vector<std::vector<Segment>> lines;
        std::vector<std::string> sources;
        std::vector<std::string> names;
        // True if the parse succeeded and the map is usable. False
        // entries are kept in the cache so we don't re-attempt a broken
        // file on every lookup.
        bool ok = false;
    };

private:
    // LRU cache. Front of `lruOrder_` is most-recently-used; back is
    // least-recently-used. `cache_` maps js_path → its node in the
    // list, so an O(1) splice moves a touched entry to the front and
    // the eviction lookup is also O(1).
    //
    // Cache key: caller-supplied js_path verbatim, no canonicalization
    // (V8 emits the same string per script id, so identity is stable).
    using LruNode = std::pair<std::string, ParsedMap>; // .first = js_path, .second = parsed map
    std::list<LruNode> lruOrder_;
    std::unordered_map<std::string, std::list<LruNode>::iterator> cache_;
    size_t cacheCapacity_;

    std::vector<std::string> searchRoots_;

    // Returns a pointer into the cached `ParsedMap` valid until the
    // next mutation (this class is not used concurrently). Loads +
    // parses on demand. Touches the LRU on hit, inserts + evicts on
    // miss.
    const ParsedMap* loadOrParse(const std::string& js_path);

    // Format-only: turn `<js_path>` into the candidate `.map` paths in
    // the order documented above. The returned strings are filesystem
    // paths to try; callers stop at the first that opens successfully.
    std::vector<std::string> candidateMapPaths(const std::string& js_path) const;
};

} // namespace topo::v8::debug

#endif // TOPO_V8_DEBUG_SOURCEMAPRESOLVER_H
