// Smoke test for containment-guard-transform tool.
//
// Spawns the production tool and verifies that restricted API call-sites
// (`eval`, `Function`, dynamic `import()`, `Reflect.*`) inside guarded
// functions are replaced by throwing IIFEs, while non-guarded functions
// are left untouched.

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
    const dir = mkdtempSync(join(tmpdir(), "topo-cgxform-"));
    const inputPath = join(dir, "request.json");
    writeFileSync(inputPath, JSON.stringify(request));
    try {
        const out = execFileSync("node", [tool, inputPath], { encoding: "utf8" });
        return JSON.parse(out);
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
}

test("eval() inside guarded function is replaced by throwing IIFE", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content: "export function helper(x: string): unknown {\n" +
                "    return eval(x);\n" +
                "}\n",
        }],
        fileGuardTargets: { "app.ts": ["helper"] },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, true);
    assert.match(r.outputContent, /Topo containment violation/);
    assert.match(r.outputContent, /eval/);
});

test("non-guarded function is untouched", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content: "export function helper(x: string): unknown {\n" +
                "    return eval(x);\n" +
                "}\n",
        }],
        fileGuardTargets: { "app.ts": [] },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, false);
    assert.equal(r.outputContent, "export function helper(x: string): unknown {\n" +
        "    return eval(x);\n}\n");
});

test("Reflect access inside guarded function is replaced", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content: "export function helper(target: object): unknown {\n" +
                "    return Reflect.get(target, 'x');\n" +
                "}\n",
        }],
        fileGuardTargets: { "app.ts": ["helper"] },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, true);
    assert.match(r.outputContent, /Topo containment violation/);
});

// Regression: rewriting any identifier named `Reflect` (etc.) without
// scope analysis over-traps user code that has deliberately shadowed
// the restricted global with a local binding. The guard must see the
// shadow and leave the reference alone.

test("shadowed Reflect via const is not trapped", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "export function helper(obj: { get(): unknown }): unknown {\n" +
                "    const Reflect = obj;\n" +
                "    return Reflect.get();\n" +
                "}\n",
        }],
        fileGuardTargets: { "app.ts": ["helper"] },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, false,
        "no rewrite should happen — Reflect is shadowed by a const");
    assert.doesNotMatch(r.outputContent, /Topo containment violation/);
    assert.match(r.outputContent, /Reflect\.get\(\)/);
});

test("shadowed Reflect via parameter is not trapped", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "export function helper(Reflect: { get(): unknown }): unknown {\n" +
                "    return Reflect.get();\n" +
                "}\n",
        }],
        fileGuardTargets: { "app.ts": ["helper"] },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, false,
        "parameter named Reflect shadows the global through the body");
    assert.doesNotMatch(r.outputContent, /Topo containment violation/);
});

test("shadowed eval (callee) is not trapped", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "export function helper(): string {\n" +
                "    const eval = (s: string) => s.toUpperCase();\n" +
                "    return eval(\"hi\");\n" +
                "}\n",
        }],
        fileGuardTargets: { "app.ts": ["helper"] },
    });
    // Note: real ECMAScript strict mode rejects `const eval`, but the
    // production tool does not type-check; we use this to verify the
    // shadow logic. Asserting on the guard behaviour, not the runtime.
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, false);
    assert.doesNotMatch(r.outputContent, /Topo containment violation/);
});

test("Reflect rebinding inside an inner block applies inside it only", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "export function helper(obj: object): void {\n" +
                "    {\n" +
                "        const Reflect = obj;\n" +
                "        void Reflect;\n" +
                "    }\n" +
                "    Reflect.get(obj, \"x\");\n" +
                "}\n",
        }],
        fileGuardTargets: { "app.ts": ["helper"] },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    // The post-block `Reflect.get` reference IS trapped (shadow ended);
    // the inner-block `Reflect` reference is NOT.
    assert.equal(r.transformed, true);
    // Exactly one trap, attached to the post-block reference.
    assert.equal(
        (r.outputContent.match(/Topo containment violation/g) ?? []).length, 1);
});

test("Reflect declared as `let` in a for-init scope shadows in that loop only", () => {
    const response = runTool({
        files: [{
            path: "app.ts",
            content:
                "export function helper(): void {\n" +
                "    for (let Reflect = 0; Reflect < 1; Reflect++) {\n" +
                "        void Reflect;\n" +
                "    }\n" +
                "    Reflect.get({}, \"x\");\n" +
                "}\n",
        }],
        fileGuardTargets: { "app.ts": ["helper"] },
    });
    assert.equal(response.success, true);
    const r = response.results[0];
    assert.equal(r.transformed, true);
    // Only the post-loop `Reflect.get` reference traps.
    assert.equal(
        (r.outputContent.match(/Topo containment violation/g) ?? []).length, 1);
});

test("missing fileGuardTargets yields success=false", () => {
    const dir = mkdtempSync(join(tmpdir(), "topo-cgxform-missing-"));
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
        assert.match(resp.error, /fileGuardTargets/);
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
});
