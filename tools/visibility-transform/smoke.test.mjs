// Smoke test for visibility-transform tool.
//
// Spawns the production tool (`node index.mjs <request.json>`) and asserts
// the round-trip request/response contract holds end-to-end. Catches
// regressions the C++ topo-build-typescript integration tests would
// otherwise have to surface indirectly.

import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const tool = join(here, "index.mjs");

function runTool(request) {
    const dir = mkdtempSync(join(tmpdir(), "topo-visxform-"));
    const inputPath = join(dir, "request.json");
    writeFileSync(inputPath, JSON.stringify(request));
    try {
        const out = execFileSync("node", [tool, inputPath], { encoding: "utf8" });
        return JSON.parse(out);
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
}

test("private declaration loses export keyword", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content: "export function helper(): number { return 1; }\n",
        }],
        fileVisibilityMaps: { "app.ts": { helper: "private" } },
    });
    assert.equal(response.success, true);
    assert.equal(response.results.length, 1);
    const r = response.results[0];
    assert.equal(r.path, "app.ts");
    assert.equal(r.transformed, true);
    assert.doesNotMatch(r.outputContent, /export function helper/);
    assert.match(r.outputContent, /function helper\(\): number/);
});

test("internal declaration gains @internal JSDoc", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content: "export function util(): void {}\n",
        }],
        fileVisibilityMaps: { "app.ts": { util: "internal" } },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, true);
    assert.match(r.outputContent, /@internal/);
    assert.match(r.outputContent, /export function util/);
});

// Regression: incremental builds re-feed the previously-transformed
// output back through the tool. Without idempotence the `@internal`
// JSDoc would stack N copies after N runs. The first run must add the
// doc; subsequent runs on the already-tagged source must be no-ops.
test("@internal annotation is idempotent across re-runs", () => {
    const first = runTool({
        files: [{
            path: "app.ts",
            content: "export function util(): void {}\n",
        }],
        fileVisibilityMaps: { "app.ts": { util: "internal" } },
    });
    assert.equal(first.success, true);
    const firstOut = first.results[0].outputContent;
    assert.equal(
        (firstOut.match(/@internal/g) ?? []).length, 1,
        "first run should add exactly one @internal");

    // Feed the first-run output back in. Expect: no new @internal added,
    // result marked transformed=false (no changes recorded).
    const second = runTool({
        files: [{ path: "app.ts", content: firstOut }],
        fileVisibilityMaps: { "app.ts": { util: "internal" } },
    });
    assert.equal(second.success, true);
    const secondOut = second.results[0].outputContent;
    assert.equal(
        (secondOut.match(/@internal/g) ?? []).length, 1,
        "second run must not stack another @internal");
    assert.equal(second.results[0].transformed, false,
        "second run reports no change because the annotation is already present");

    // Third run for good measure — still exactly one.
    const third = runTool({
        files: [{ path: "app.ts", content: secondOut }],
        fileVisibilityMaps: { "app.ts": { util: "internal" } },
    });
    assert.equal(
        (third.results[0].outputContent.match(/@internal/g) ?? []).length, 1,
        "third run must still leave exactly one @internal");
});

// A hand-authored `/** @internal */` JSDoc on the source must also
// suppress the append — declaration-driven re-tagging should not
// duplicate an annotation the author already wrote.
test("@internal annotation is skipped when source already has it", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content: "/** @internal */\nexport function util(): void {}\n",
        }],
        fileVisibilityMaps: { "app.ts": { util: "internal" } },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(
        (r.outputContent.match(/@internal/g) ?? []).length, 1,
        "hand-authored @internal must not be doubled");
    assert.equal(r.transformed, false);
});

test("public declaration is unchanged", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content: "export function api(): boolean { return true; }\n",
        }],
        fileVisibilityMaps: { "app.ts": { api: "public" } },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.match(r.outputContent, /export function api/);
    assert.doesNotMatch(r.outputContent, /@internal/);
});

test("unlisted declaration is untouched (file-scoped map)", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content: "export function listed(): number { return 1; }\n" +
                "export function unlisted(): number { return 2; }\n",
        }],
        fileVisibilityMaps: { "app.ts": { listed: "private" } },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.doesNotMatch(r.outputContent, /export function listed/);
    assert.match(r.outputContent, /export function unlisted/);
});

test("invalid JSON yields success=false with error", () => {
    const dir = mkdtempSync(join(tmpdir(), "topo-visxform-bad-"));
    const inputPath = join(dir, "bad.json");
    writeFileSync(inputPath, "{not valid json");
    try {
        let exitCode = 0;
        let out = "";
        try {
            out = execFileSync("node", [tool, inputPath], { encoding: "utf8" });
        } catch (err) {
            exitCode = err.status;
            out = err.stdout?.toString() ?? "";
        }
        assert.notEqual(exitCode, 0);
        const resp = JSON.parse(out);
        assert.equal(resp.success, false);
        assert.match(resp.error, /invalid JSON/);
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

// Input-size cap: ensure the
// per-file content cap fires as a structured error rather than letting
// JSON.parse OOM on a multi-GB string. Drive the cap down with
// TOPO_V8_TRANSFORM_MAX_FILE_CONTENT_BYTES so the test stays cheap.
test("oversize per-file content yields structured size-cap error", () => {
    const dir = mkdtempSync(join(tmpdir(), "topo-visxform-cap-"));
    const inputPath = join(dir, "req.json");
    const big = "x".repeat(2048);
    writeFileSync(inputPath, JSON.stringify({
        files: [{ path: "a.ts", content: big }],
        fileVisibilityMaps: { "a.ts": {} },
    }));
    try {
        let exitCode = 0;
        let out = "";
        try {
            out = execFileSync("node", [tool, inputPath], {
                encoding: "utf8",
                env: { ...process.env,
                       TOPO_V8_TRANSFORM_MAX_FILE_CONTENT_BYTES: "1024" },
            });
        } catch (err) {
            exitCode = err.status;
            out = err.stdout?.toString() ?? "";
        }
        assert.notEqual(exitCode, 0);
        const resp = JSON.parse(out);
        assert.equal(resp.success, false);
        assert.match(resp.error, /exceeds cap/);
        assert.match(resp.error, /TOPO_V8_TRANSFORM_MAX_FILE_CONTENT_BYTES/);
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

// Input-size cap: the input-size
// cap fires before JSON.parse, so a giant request file is rejected on
// stat rather than read into memory.
test("oversize input file yields structured input-size error", () => {
    const dir = mkdtempSync(join(tmpdir(), "topo-visxform-inputcap-"));
    const inputPath = join(dir, "req.json");
    writeFileSync(inputPath, "X".repeat(2048));
    try {
        let exitCode = 0;
        let out = "";
        try {
            out = execFileSync("node", [tool, inputPath], {
                encoding: "utf8",
                env: { ...process.env,
                       TOPO_V8_TRANSFORM_MAX_INPUT_BYTES: "1024" },
            });
        } catch (err) {
            exitCode = err.status;
            out = err.stdout?.toString() ?? "";
        }
        assert.notEqual(exitCode, 0);
        const resp = JSON.parse(out);
        assert.equal(resp.success, false);
        assert.match(resp.error, /exceeds cap/);
        assert.match(resp.error, /TOPO_V8_TRANSFORM_MAX_INPUT_BYTES/);
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});

test("missing fileVisibilityMaps yields success=false", () => {
    const dir = mkdtempSync(join(tmpdir(), "topo-visxform-missing-"));
    const inputPath = join(dir, "req.json");
    writeFileSync(inputPath, JSON.stringify({
        files: [{ path: "a.ts", content: "export function x(){}" }],
        // intentionally omit fileVisibilityMaps
    }));
    try {
        let exitCode = 0;
        let out = "";
        try {
            out = execFileSync("node", [tool, inputPath], { encoding: "utf8" });
        } catch (err) {
            exitCode = err.status;
            out = err.stdout?.toString() ?? "";
        }
        assert.notEqual(exitCode, 0);
        const resp = JSON.parse(out);
        assert.equal(resp.success, false);
        assert.match(resp.error, /fileVisibilityMaps/);
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});
