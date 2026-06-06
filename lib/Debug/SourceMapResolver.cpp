// Source Map v3 resolver implementation.
//
// VLQ-base64 decoding is implemented inline to avoid pulling in a
// dedicated dependency. The mapping segment grammar is a sequence of
// 1, 4, or 5 VLQ integers separated by `,` within a line and `;`
// between generated lines. We decode strictly per the Source Map v3 spec
// surface any structural inconsistency as a parse failure (cached as
// `ok = false`).

#include "SourceMapResolver.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace topo::v8::debug {

namespace {

namespace fs = std::filesystem;

// Decode one base64 VLQ character. Returns -1 for invalid input.
int base64Char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decode one VLQ-encoded signed integer starting at `pos` in
// `mappings`. Advances `pos` past the last consumed character. Returns
// true on success; on failure leaves `pos` at the offending character
// and the caller fails the entire parse.
//
// Encoding: each 6-bit group encodes 5 data bits + 1 continuation bit
// (the MSB). The least-significant data bit of the first group is the
// sign bit. Groups are emitted in little-endian order.
bool decodeVLQ(const std::string& s, size_t& pos, int& outValue) {
    int result = 0;
    int shift = 0;
    bool continuation = true;
    while (continuation) {
        if (pos >= s.size()) return false;
        int digit = base64Char(s[pos++]);
        if (digit < 0) return false;
        continuation = (digit & 0x20) != 0;
        int data = digit & 0x1F;
        result |= (data << shift);
        shift += 5;
        // Guard against malformed runaway VLQs (each int fits in 32 bits
        // in any valid map; refuse beyond ~7 groups).
        if (shift > 35) return false;
    }
    // Recover signed value: LSB is the sign bit.
    bool negative = (result & 1) != 0;
    int magnitude = result >> 1;
    outValue = negative ? -magnitude : magnitude;
    return true;
}

// Parse a Source Map v3 JSON document from `text` into `out`. Sets
// `out.ok` true on success. Tolerates absent `sourceRoot` / `names`.
// Rejects `sections`-style index maps (we don't need them yet).
bool parseSourceMap(const std::string& text,
                    SourceMapResolver::ParsedMap& out) {
    out.ok = false;
    out.lines.clear();
    out.sources.clear();
    out.names.clear();

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        return false;
    }
    if (!doc.is_object()) return false;
    // Honor the resolver's documented no-throw contract: `version` may exist
    // with a non-integer JSON type (`"3"`, `3.0`, `null`). `get<int>()` on a
    // wrong-typed value throws `json::type_error`; gate on an integral type
    // so a malformed `.map` fails closed (returns false) instead of unwinding.
    if (!doc.contains("version") || !doc["version"].is_number_integer() ||
        doc["version"].get<int>() != 3) {
        return false;
    }
    if (!doc.contains("mappings") || !doc["mappings"].is_string()) {
        return false;
    }
    if (!doc.contains("sources") || !doc["sources"].is_array()) {
        return false;
    }
    std::string sourceRoot;
    if (doc.contains("sourceRoot") && doc["sourceRoot"].is_string()) {
        sourceRoot = doc["sourceRoot"].get<std::string>();
        // Per spec the join is `sourceRoot + sources[i]` with sourceRoot
        // optionally ending in `/`. We preserve the field verbatim and
        // do a simple concatenation below; downstream consumers expect
        // the same shape tsc emits.
    }
    for (const auto& s : doc["sources"]) {
        if (!s.is_string()) {
            // The spec allows `null` entries; we treat them as empty.
            out.sources.push_back("");
            continue;
        }
        std::string entry = s.get<std::string>();
        if (!sourceRoot.empty()) {
            // Avoid double-slash when both halves carry one.
            if (!sourceRoot.empty() && sourceRoot.back() == '/' &&
                !entry.empty() && entry.front() == '/') {
                entry = sourceRoot + entry.substr(1);
            } else if (!sourceRoot.empty() && sourceRoot.back() != '/' &&
                       (entry.empty() || entry.front() != '/')) {
                entry = sourceRoot + "/" + entry;
            } else {
                entry = sourceRoot + entry;
            }
        }
        out.sources.push_back(std::move(entry));
    }
    if (doc.contains("names") && doc["names"].is_array()) {
        for (const auto& n : doc["names"]) {
            if (n.is_string()) out.names.push_back(n.get<std::string>());
            else out.names.push_back("");
        }
    }

    const std::string& mappings = doc["mappings"].get_ref<const std::string&>();

    // Running deltas: VLQ deltas are relative within a map but the
    // `generatedColumn` field resets to 0 at every new generated line
    // (i.e. every `;`). The other four fields persist across lines.
    int srcIdx = 0, srcLine = 0, srcCol = 0, nameIdx = 0;

    out.lines.emplace_back();
    size_t pos = 0;
    int genCol = 0;
    while (pos < mappings.size()) {
        char c = mappings[pos];
        if (c == ';') {
            out.lines.emplace_back();
            genCol = 0;
            ++pos;
            continue;
        }
        if (c == ',') {
            ++pos;
            continue;
        }
        // Decode 1, 4, or 5 fields. The first VLQ is the generatedColumn
        // delta and is always present.
        int dGenCol = 0;
        if (!decodeVLQ(mappings, pos, dGenCol)) return false;
        genCol += dGenCol;

        SourceMapResolver::Segment seg;
        seg.generatedColumn = genCol;
        seg.sourceIndex = -1;

        // Peek: if we hit `,` / `;` / EOF after the first field, this is
        // a column-only segment. Otherwise, read 3 more fields, and
        // optionally a 5th for the name.
        if (pos < mappings.size() && mappings[pos] != ',' &&
            mappings[pos] != ';') {
            int dSrcIdx = 0, dSrcLine = 0, dSrcCol = 0;
            if (!decodeVLQ(mappings, pos, dSrcIdx)) return false;
            if (!decodeVLQ(mappings, pos, dSrcLine)) return false;
            if (!decodeVLQ(mappings, pos, dSrcCol)) return false;
            srcIdx += dSrcIdx;
            srcLine += dSrcLine;
            srcCol += dSrcCol;
            seg.sourceIndex = srcIdx;
            seg.sourceLine = srcLine;
            seg.sourceColumn = srcCol;

            if (pos < mappings.size() && mappings[pos] != ',' &&
                mappings[pos] != ';') {
                int dNameIdx = 0;
                if (!decodeVLQ(mappings, pos, dNameIdx)) return false;
                nameIdx += dNameIdx;
                seg.nameIndex = nameIdx;
            }
        }
        // Only keep mapped segments — column-only entries can't answer
        // a lookup (no source position), so we skip them entirely.
        if (seg.sourceIndex >= 0 &&
            static_cast<size_t>(seg.sourceIndex) < out.sources.size()) {
            out.lines.back().push_back(seg);
        }
    }
    out.ok = true;
    return true;
}

} // namespace

namespace {

/// Pick the cache capacity once at construction time. Honour the
/// `TOPO_V8_SOURCEMAP_CACHE` env var when set to a positive integer;
/// otherwise use the default soft cap.
size_t pickCacheCapacity() {
    constexpr size_t kDefaultCap = 256;
    const char* env = std::getenv("TOPO_V8_SOURCEMAP_CACHE");
    if (env == nullptr || *env == '\0') return kDefaultCap;
    try {
        long v = std::stol(env);
        if (v > 0) return static_cast<size_t>(v);
    } catch (...) {
        // fall through to the default
    }
    return kDefaultCap;
}

} // namespace

SourceMapResolver::SourceMapResolver()
    : cacheCapacity_(pickCacheCapacity()) {}
SourceMapResolver::~SourceMapResolver() = default;

void SourceMapResolver::addSearchRoot(std::string dir) {
    searchRoots_.push_back(std::move(dir));
}

std::vector<std::string> SourceMapResolver::candidateMapPaths(
    const std::string& js_path) const {
    std::vector<std::string> out;
    out.push_back(js_path + ".map");
    fs::path p(js_path);
    if (p.extension() == ".js" || p.extension() == ".mjs" ||
        p.extension() == ".cjs") {
        fs::path stem = p;
        stem.replace_extension("");
        out.push_back(stem.string() + ".js.map");
    }
    std::string basename = p.filename().string();
    for (const auto& root : searchRoots_) {
        fs::path rp(root);
        out.push_back((rp / (basename + ".map")).string());
        // Also accept `<root>/<stem>.js.map` so a caller registering the
        // dist directory matches both naming conventions.
        if (p.extension() == ".js" || p.extension() == ".mjs" ||
            p.extension() == ".cjs") {
            fs::path stem(basename);
            stem.replace_extension("");
            out.push_back((rp / (stem.string() + ".js.map")).string());
        }
    }
    return out;
}

const SourceMapResolver::ParsedMap* SourceMapResolver::loadOrParse(
    const std::string& js_path) {
    // Cache hit: splice the node to the front so it stays MRU.
    auto it = cache_.find(js_path);
    if (it != cache_.end()) {
        lruOrder_.splice(lruOrder_.begin(), lruOrder_, it->second);
        return &it->second->second;
    }
    // Cache miss: parse the map (or record an `ok=false` sentinel so
    // we do not re-attempt a broken file on every lookup).
    ParsedMap pm;
    pm.ok = false;
    for (const auto& cand : candidateMapPaths(js_path)) {
        std::error_code ec;
        if (!fs::exists(cand, ec)) continue;
        std::ifstream in(cand);
        if (!in) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        if (parseSourceMap(ss.str(), pm)) break;
        // Reset partial state and keep trying the remaining candidates.
        pm = ParsedMap{};
        pm.ok = false;
    }
    // Insert at the front of the LRU. The `cache_` map points at the
    // list node so later splice operations stay O(1).
    lruOrder_.emplace_front(js_path, std::move(pm));
    cache_.emplace(js_path, lruOrder_.begin());

    // Evict back-of-LRU entries until we are within the soft cap.
    while (cache_.size() > cacheCapacity_ && !lruOrder_.empty()) {
        cache_.erase(lruOrder_.back().first);
        lruOrder_.pop_back();
    }
    return &lruOrder_.front().second;
}

std::optional<ResolvedFrame> SourceMapResolver::resolve(
    const std::string& js_path, int line_1indexed, int column_1indexed) {
    if (line_1indexed < 1) return std::nullopt;
    const ParsedMap* m = loadOrParse(js_path);
    if (m == nullptr || !m->ok) return std::nullopt;
    int gline = line_1indexed - 1;
    if (gline < 0 || static_cast<size_t>(gline) >= m->lines.size()) {
        return std::nullopt;
    }
    const auto& segs = m->lines[gline];
    if (segs.empty()) return std::nullopt;
    int gcol = column_1indexed > 0 ? column_1indexed - 1 : 0;
    // Find rightmost segment with generatedColumn <= gcol.
    auto it = std::upper_bound(
        segs.begin(), segs.end(), gcol,
        [](int v, const Segment& s) { return v < s.generatedColumn; });
    if (it == segs.begin()) {
        // The first segment on this line starts at a higher column than
        // the request. V8 likes to report column 0 (or 1 post-bump) for
        // function-entry frames; rather than reject, snap to the first
        // segment — it's the closest we can do.
        const Segment& seg = segs.front();
        ResolvedFrame rf;
        rf.source_path = m->sources[seg.sourceIndex];
        rf.line_1indexed = seg.sourceLine + 1;
        rf.column_1indexed = seg.sourceColumn + 1;
        if (seg.nameIndex >= 0 &&
            static_cast<size_t>(seg.nameIndex) < m->names.size()) {
            rf.name = m->names[seg.nameIndex];
        }
        return rf;
    }
    --it;
    const Segment& seg = *it;
    if (seg.sourceIndex < 0 ||
        static_cast<size_t>(seg.sourceIndex) >= m->sources.size()) {
        return std::nullopt;
    }
    ResolvedFrame rf;
    rf.source_path = m->sources[seg.sourceIndex];
    rf.line_1indexed = seg.sourceLine + 1;
    rf.column_1indexed = seg.sourceColumn + 1;
    if (seg.nameIndex >= 0 &&
        static_cast<size_t>(seg.nameIndex) < m->names.size()) {
        rf.name = m->names[seg.nameIndex];
    }
    return rf;
}

} // namespace topo::v8::debug
