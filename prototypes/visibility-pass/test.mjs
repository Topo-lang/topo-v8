// Round-trip test for the VisibilityPass prototype.
// Verifies: public → unchanged; internal → @internal JSDoc added;
// private → export keyword stripped; unlisted → unchanged.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import { rewrite } from "./index.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const fixturesDir = join(here, "fixtures");

const inputText = readFileSync(join(fixturesDir, "sample.input.ts"), "utf8");
const visibilityMap = JSON.parse(
    readFileSync(join(fixturesDir, "sample.visibility.json"), "utf8"));

const output = rewrite(inputText, visibilityMap, "sample.input.ts");

test("public declaration is unchanged", () => {
    assert.match(output, /export function publicApi\(x: number\): number \{/);
    assert.doesNotMatch(output.split("publicApi")[0], /@internal/);
});

test("internal declaration keeps export and gains @internal JSDoc", () => {
    const idx = output.indexOf("function internalHelper");
    const before = output.slice(0, idx);
    assert.match(output, /export function internalHelper/);
    assert.match(before, /@internal/);
});

test("private declaration loses the export keyword", () => {
    assert.doesNotMatch(output, /export function privateImpl/);
    assert.match(output, /function privateImpl\(x: number\): number \{/);
});

test("public const is unchanged", () => {
    assert.match(output, /export const PUBLIC_CONST = 42/);
});

test("unlisted declaration is left untouched", () => {
    assert.match(output, /function notListed\(\): void \{/);
});

test("rewritten output is parseable TypeScript", async () => {
    const ts = (await import("typescript")).default;
    const sf = ts.createSourceFile("out.ts", output, ts.ScriptTarget.ES2022, true);
    // No syntactic diagnostics on a freshly parsed file.
    const program = ts.createProgram({
        rootNames: ["out.ts"],
        options: { noEmit: true, allowJs: false, strict: false, skipLibCheck: true },
        host: {
            getSourceFile: (name) => name === "out.ts" ? sf : undefined,
            getDefaultLibFileName: () => "lib.d.ts",
            writeFile: () => {},
            getCurrentDirectory: () => "/",
            getCanonicalFileName: (f) => f,
            useCaseSensitiveFileNames: () => true,
            getNewLine: () => "\n",
            fileExists: (f) => f === "out.ts",
            readFile: () => undefined,
            getDirectories: () => [],
        },
    });
    const diags = program.getSyntacticDiagnostics(sf);
    assert.deepEqual(diags, [], `Syntactic diagnostics on rewritten output: ${
        diags.map(d => ts.flattenDiagnosticMessageText(d.messageText, "\n")).join("; ")}`);
});
