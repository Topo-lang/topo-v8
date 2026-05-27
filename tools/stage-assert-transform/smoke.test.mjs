// Smoke test for stage-assert-transform tool.
//
// Spawns the production tool and verifies that direct call-sites of
// stage-mapped callees inside guarded host functions get wrapped in an
// IIFE that asserts monotonic stage order at runtime.

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
    const dir = mkdtempSync(join(tmpdir(), "topo-sxform-"));
    const inputPath = join(dir, "request.json");
    writeFileSync(inputPath, JSON.stringify(request));
    try {
        const out = execFileSync("node", [tool, inputPath], { encoding: "utf8" });
        return JSON.parse(out);
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
}

test("staged callees inside mapped function get wrapped", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "function fetch(): number { return 1; }\n" +
                "function parse(x: number): number { return x; }\n" +
                "function emit(x: number): void {}\n" +
                "export function pipeline(): void {\n" +
                "    const a = fetch();\n" +
                "    const b = parse(a);\n" +
                "    emit(b);\n" +
                "}\n",
        }],
        fileStageMaps: {
            "app.ts": {
                pipeline: [
                    ["fetch", "parse"],
                    ["emit"],
                ],
            },
        },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, true);
    assert.match(r.outputContent, /__topoStage/);
    assert.match(r.outputContent, /Topo stage assertion/);
});

test("non-mapped function is untouched", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "function fetch(): number { return 1; }\n" +
                "export function other(): number { return fetch(); }\n",
        }],
        fileStageMaps: { "app.ts": {} },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, false);
});

test("user-bound __topoStage forces hashed counter identifier", () => {
    // Pins issue stage-assert-transform-injected-identifiers-collide-silently.
    // When the user's function body already binds __topoStage, the
    // transform must rename the injected counter to a hashed form
    // instead of silently shadowing the user's binding.
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "function fetch(): number { return 1; }\n" +
                "function emit(x: number): void {}\n" +
                "export function pipeline(): void {\n" +
                "    let __topoStage = 42;\n" +
                "    const a = fetch();\n" +
                "    emit(a);\n" +
                "    console.log(__topoStage);\n" +
                "}\n",
        }],
        fileStageMaps: {
            "app.ts": {
                pipeline: [
                    ["fetch"],
                    ["emit"],
                ],
            },
        },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, true);
    // The user's original __topoStage binding survives.
    assert.match(r.outputContent, /let __topoStage = 42;/);
    // The injected counter uses a hashed suffix.
    assert.match(r.outputContent, /let __topoStage\$[0-9a-f]{8} = 0;/);
    // The IIFE references the hashed counter, not the bare one.
    assert.match(r.outputContent, /__topoStage\$[0-9a-f]{8} = Math\.max/);
    // The collision-avoided change is recorded in `changes`.
    assert.ok(r.changes.some(c => c.includes("avoid collision")),
        "expected an 'avoid collision' change entry");
});

test("user-bound __need also forces hashed identifiers", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "function fetch(): number { return 1; }\n" +
                "export function pipeline(): void {\n" +
                "    const __need = 'unrelated';\n" +
                "    const a = fetch();\n" +
                "    console.log(__need);\n" +
                "}\n",
        }],
        fileStageMaps: {
            "app.ts": {
                pipeline: [["fetch"]],
            },
        },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, true);
    // Binding either name triggers the rename of both.
    assert.match(r.outputContent, /__need\$[0-9a-f]{8}/);
    assert.match(r.outputContent, /__topoStage\$[0-9a-f]{8}/);
});

test("no collision keeps the bare identifier names", () => {
    // When the user body does NOT bind either name, the bare
    // __topoStage / __need are still used (back-compat with existing
    // fixtures and source readability).
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "function fetch(): number { return 1; }\n" +
                "export function pipeline(): void {\n" +
                "    const a = fetch();\n" +
                "}\n",
        }],
        fileStageMaps: {
            "app.ts": {
                pipeline: [["fetch"]],
            },
        },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.match(r.outputContent, /let __topoStage = 0;/);
    assert.doesNotMatch(r.outputContent, /__topoStage\$[0-9a-f]{8}/);
});

test("parameter named __topoStage forces hashed counter identifier", () => {
    // Pins issue stage-assert-collision-misses-function-parameters.
    // Before the fix, chooseInjectedIdentifiers walked only the function
    // body when scanning for collisions with the default injected names
    // — a function that took __topoStage as a parameter still received a
    // bare `let __topoStage = 0;` into its body, redeclaring the parameter
    // (SyntaxError in strict mode). After the fix, parameter names
    // participate in collision detection and the hashed form is used.
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "function fetch(): number { return 1; }\n" +
                "export function pipeline(__topoStage: number): void {\n" +
                "    const a = fetch();\n" +
                "    console.log(__topoStage, a);\n" +
                "}\n",
        }],
        fileStageMaps: {
            "app.ts": {
                pipeline: [["fetch"]],
            },
        },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, true);
    // The bare `let __topoStage = 0;` must NOT be emitted — it would
    // redeclare the parameter.
    assert.doesNotMatch(r.outputContent, /let __topoStage = 0;/);
    // The hashed form must be emitted instead.
    assert.match(r.outputContent, /let __topoStage\$[0-9a-f]{8} = 0;/);
    // The IIFE references the hashed counter.
    assert.match(r.outputContent, /__topoStage\$[0-9a-f]{8} = Math\.max/);
    // The collision-avoided change is recorded.
    assert.ok(r.changes.some(c => c.includes("avoid collision")),
        "expected an 'avoid collision' change entry");
});

test("parameter named __need also forces hashed identifiers", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "function fetch(): number { return 1; }\n" +
                "export function pipeline(__need: string): void {\n" +
                "    const a = fetch();\n" +
                "    console.log(__need, a);\n" +
                "}\n",
        }],
        fileStageMaps: {
            "app.ts": {
                pipeline: [["fetch"]],
            },
        },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, true);
    // Either name colliding renames both, matching the existing
    // body-collision contract.
    assert.match(r.outputContent, /__topoStage\$[0-9a-f]{8}/);
    assert.match(r.outputContent, /__need\$[0-9a-f]{8}/);
    assert.ok(r.changes.some(c => c.includes("avoid collision")));
});

test("parameter list with no collision keeps bare identifier names", () => {
    // Positive case for the parameter-aware collision check: when no
    // parameter and no body identifier clashes with the defaults, the
    // bare names survive.
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "function fetch(): number { return 1; }\n" +
                "export function pipeline(x: number, y: string): void {\n" +
                "    const a = fetch();\n" +
                "    console.log(x, y, a);\n" +
                "}\n",
        }],
        fileStageMaps: {
            "app.ts": {
                pipeline: [["fetch"]],
            },
        },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, true);
    assert.match(r.outputContent, /let __topoStage = 0;/);
    assert.doesNotMatch(r.outputContent, /__topoStage\$[0-9a-f]{8}/);
    assert.ok(!r.changes.some(c => c.includes("avoid collision")),
        "did not expect an 'avoid collision' change entry");
});

test("missing fileStageMaps yields success=false", () => {
    const dir = mkdtempSync(join(tmpdir(), "topo-sxform-missing-"));
    const inputPath = join(dir, "req.json");
    writeFileSync(inputPath, JSON.stringify({
        files: [{ path: "a.ts", content: "function x(){}" }],
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
        assert.match(resp.error, /fileStageMaps/);
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});
