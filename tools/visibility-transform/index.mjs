// topo-v8/tools/visibility-transform — VisibilityPass production tool.
//
// Reads a JSON request from stdin, applies visibility rewrite to each file
// using the TypeScript Compiler API, and writes a JSON response to stdout.
//
// Protocol:
//   stdin  → {
//              "files": [{"path":"src/app.ts","content":"..."}],
//              "fileVisibilityMaps": {
//                "src/app.ts": {"helper":"private","internalUtil":"internal"}
//              }
//            }
//   stdout → {"success":true,"results":[{"path":"src/app.ts","transformed":true,
//              "outputContent":"...","changes":["applied visibility: private"]}]}
//
// Each visibility map is file-scoped — a `private` mapping for `helper` in
// `src/app.ts` does NOT affect a `helper` in `src/util.ts`. This sidesteps
// the cross-namespace simple-name collision a global map would have.

import ts from "typescript";

// ---------------------------------------------------------------------------
// Core transform logic (productionized from prototypes/visibility-pass)
// ---------------------------------------------------------------------------

const INTERNAL_DOC = "*\n * @internal\n ";

function declaredName(node) {
    if (
        ts.isFunctionDeclaration(node) ||
        ts.isClassDeclaration(node) ||
        ts.isInterfaceDeclaration(node) ||
        ts.isTypeAliasDeclaration(node) ||
        ts.isEnumDeclaration(node) ||
        ts.isModuleDeclaration(node)
    ) {
        return node.name?.getText();
    }
    if (ts.isVariableStatement(node)) {
        const decls = node.declarationList.declarations;
        if (decls.length === 1 && ts.isIdentifier(decls[0].name)) {
            return decls[0].name.text;
        }
    }
    return undefined;
}

// Returns the raw text of each leading comment trivia attached to `node`
// in the original source. Used by the @internal idempotence check: when
// the previous run baked a `/** @internal */` JSDoc into the printed
// output, a re-parse of that output sees the comment as ordinary source
// trivia (not as synthetic), so `ts.getSyntheticLeadingComments` returns
// empty. Scanning the source-text trivia ranges covers that case.
function leadingCommentsAt(sourceText, node) {
    const ranges = ts.getLeadingCommentRanges(
        sourceText, node.getFullStart()) ?? [];
    return ranges.map(r => sourceText.slice(r.pos, r.end));
}

function withModifiers(node, mods, factory) {
    if (ts.isFunctionDeclaration(node)) {
        return factory.updateFunctionDeclaration(
            node, mods, node.asteriskToken, node.name, node.typeParameters,
            node.parameters, node.type, node.body);
    }
    if (ts.isClassDeclaration(node)) {
        return factory.updateClassDeclaration(
            node, mods, node.name, node.typeParameters,
            node.heritageClauses, node.members);
    }
    if (ts.isInterfaceDeclaration(node)) {
        return factory.updateInterfaceDeclaration(
            node, mods, node.name, node.typeParameters,
            node.heritageClauses, node.members);
    }
    if (ts.isTypeAliasDeclaration(node)) {
        return factory.updateTypeAliasDeclaration(
            node, mods, node.name, node.typeParameters, node.type);
    }
    if (ts.isEnumDeclaration(node)) {
        return factory.updateEnumDeclaration(
            node, mods, node.name, node.members);
    }
    if (ts.isVariableStatement(node)) {
        return factory.updateVariableStatement(
            node, mods, node.declarationList);
    }
    return node;
}

function withoutExport(modifiers) {
    if (!modifiers) return undefined;
    const filtered = modifiers.filter(
        m =>
            m.kind !== ts.SyntaxKind.ExportKeyword &&
            m.kind !== ts.SyntaxKind.DefaultKeyword);
    return filtered.length ? filtered : undefined;
}

function rewrite(sourceText, visibilityMap, fileName = "input.ts") {
    const source = ts.createSourceFile(
        fileName, sourceText, ts.ScriptTarget.ES2022,
        /*setParentNodes*/ true);

    // Reports back whether the AST was actually modified. The printer
    // normalises whitespace (e.g. drops blank lines), so a plain string
    // diff of the output against the source can't tell "real change" from
    // "cosmetic reflow". Callers should consult `changes` instead.
    const changes = [];

    const transformer = (context) => (root) => {
        const factory = context.factory;
        const visit = (node) => {
            if (node.parent && node.parent.kind === ts.SyntaxKind.SourceFile) {
                const name = declaredName(node);
                const action = name ? visibilityMap[name] : undefined;
                if (action) {
                    const isExported = (node.modifiers ?? []).some(
                        m => m.kind === ts.SyntaxKind.ExportKeyword);
                    if (action === "private" && isExported) {
                        const newMods = withoutExport(node.modifiers);
                        changes.push(`stripped export from ${name}`);
                        return withModifiers(node, newMods, factory);
                    }
                    if (action === "internal") {
                        // Idempotence: if an existing leading comment
                        // already mentions @internal (synthetic from a
                        // prior run of this pass, or hand-authored
                        // JSDoc), skip the append. Without this guard,
                        // every incremental build would stack another
                        // copy of the `@internal` doc-comment.
                        const existing =
                            ts.getSyntheticLeadingComments(node) ?? [];
                        const alreadyInternal = existing.some(
                            c => typeof c.text === "string" &&
                                 /@internal\b/.test(c.text));
                        const sourceLeading = leadingCommentsAt(
                            sourceText, node);
                        const sourceHasInternal = sourceLeading.some(
                            c => /@internal\b/.test(c));
                        if (alreadyInternal || sourceHasInternal) {
                            return node;
                        }
                        ts.setSyntheticLeadingComments(node, [...existing, {
                            kind: ts.SyntaxKind.MultiLineCommentTrivia,
                            text: INTERNAL_DOC,
                            hasTrailingNewLine: true,
                            pos: -1, end: -1,
                        }]);
                        changes.push(`added @internal to ${name}`);
                        return node;
                    }
                }
            }
            return ts.visitEachChild(node, visit, context);
        };
        return ts.visitEachChild(root, visit, context);
    };

    const result = ts.transform(source, [transformer]);
    const printer = ts.createPrinter({ newLine: ts.NewLineKind.LineFeed });
    const out = printer.printFile(result.transformed[0]);
    result.dispose();

    // Syntactic re-parse of the printer output. Catches structural breakage
    // (modifier removal landing on the wrong node, malformed JSDoc, etc.)
    // before we write a non-compilable file to dist/. This is parse-only,
    // not type-checking — fast and adds no module-resolution dependency.
    const reparsed = ts.createSourceFile(
        fileName, out, ts.ScriptTarget.ES2022, /*setParentNodes*/ false);
    const diags = reparsed.parseDiagnostics ?? [];
    if (diags.length > 0) {
        const messages = diags.map(d => ts.flattenDiagnosticMessageText(
            d.messageText, "\n")).join("; ");
        throw new Error(`transformed source has syntax errors: ${messages}`);
    }

    return { output: out, changes };
}

// ---------------------------------------------------------------------------
// Batch runner
// ---------------------------------------------------------------------------

function processRequest(request) {
    const { files, fileVisibilityMaps } = request;
    const results = [];

    for (const file of files) {
        try {
            const perFileMap = fileVisibilityMaps[file.path] ?? {};
            // Fast-path: no directives → no rewrite, no output rewrite.
            // Also avoids the printer reflowing untouched files into spurious
            // "changed" output (it normalises whitespace).
            if (Object.keys(perFileMap).length === 0) {
                results.push({
                    path: file.path,
                    transformed: false,
                    outputContent: file.content,
                    changes: [],
                });
                continue;
            }
            const { output, changes } = rewrite(
                file.content, perFileMap, file.path);
            if (changes.length > 0) {
                results.push({
                    path: file.path,
                    transformed: true,
                    outputContent: output,
                    changes,
                });
            } else {
                results.push({
                    path: file.path,
                    transformed: false,
                    outputContent: file.content,
                    changes: [],
                });
            }
        } catch (err) {
            results.push({
                path: file.path,
                transformed: false,
                error: err.message,
            });
        }
    }

    const allOk = results.every(r => !r.error);
    return { success: allOk, results };
}

// ---------------------------------------------------------------------------
// Main — accepts JSON file path as argv[1], or reads from stdin
// ---------------------------------------------------------------------------

// Input size caps (audit issue node-transform-tools-no-input-size-limit).
// Each cap is overridable via env so a legitimately large megaproject build
// can lift the bound; the cap defaults exist only so a runaway/malicious
// input surfaces as a structured error instead of an OOM crash.
const DEFAULT_MAX_INPUT_BYTES = 64 * 1024 * 1024;   // 64 MiB total request
const DEFAULT_MAX_FILES = 10_000;                     // entries in files[]
const DEFAULT_MAX_FILE_CONTENT_BYTES = 16 * 1024 * 1024; // 16 MiB per file

function envInt(name, fallback) {
    const v = process.env[name];
    if (!v) return fallback;
    const n = Number.parseInt(v, 10);
    return Number.isFinite(n) && n > 0 ? n : fallback;
}

const MAX_INPUT_BYTES = envInt("TOPO_V8_TRANSFORM_MAX_INPUT_BYTES",
                                DEFAULT_MAX_INPUT_BYTES);
const MAX_FILES = envInt("TOPO_V8_TRANSFORM_MAX_FILES",
                          DEFAULT_MAX_FILES);
const MAX_FILE_CONTENT_BYTES = envInt(
    "TOPO_V8_TRANSFORM_MAX_FILE_CONTENT_BYTES",
    DEFAULT_MAX_FILE_CONTENT_BYTES);

function readStdinCapped(maxBytes) {
    return new Promise((resolve, reject) => {
        const chunks = [];
        let total = 0;
        let overflowed = false;
        process.stdin.on("data", (chunk) => {
            if (overflowed) return;
            total += chunk.length;
            if (total > maxBytes) {
                overflowed = true;
                // Drain stdin so the producer doesn't EPIPE before we
                // emit the error envelope.
                process.stdin.pause();
                resolve({ overflow: true, total });
                return;
            }
            chunks.push(chunk);
        });
        process.stdin.on("end", () => {
            if (overflowed) return;
            resolve({
                overflow: false,
                text: Buffer.concat(chunks).toString("utf8"),
            });
        });
        process.stdin.on("error", reject);
    });
}

function emitError(message) {
    process.stderr.write(`error: ${message}\n`);
    process.stdout.write(JSON.stringify({
        success: false,
        error: message,
    }));
    process.exit(1);
}

async function main() {
    let input;

    if (process.argv.length >= 3) {
        // File path mode — invoked as: node index.mjs <request.json>
        const fs = await import("node:fs");
        const stat = fs.statSync(process.argv[2]);
        if (stat.size > MAX_INPUT_BYTES) {
            emitError(
                `request file ${process.argv[2]} is ${stat.size} bytes, ` +
                `exceeds cap ${MAX_INPUT_BYTES} ` +
                `(set TOPO_V8_TRANSFORM_MAX_INPUT_BYTES to raise)`);
        }
        input = fs.readFileSync(process.argv[2], "utf8");
    } else {
        // Stdin mode — invoked as: cat request.json | node index.mjs
        const r = await readStdinCapped(MAX_INPUT_BYTES);
        if (r.overflow) {
            emitError(
                `stdin request exceeded ${MAX_INPUT_BYTES} bytes ` +
                `(set TOPO_V8_TRANSFORM_MAX_INPUT_BYTES to raise)`);
        }
        input = r.text;
    }

    let request;
    try {
        request = JSON.parse(input);
    } catch (err) {
        emitError(`invalid JSON input: ${err.message}`);
    }

    if (!request.files || !Array.isArray(request.files)) {
        emitError("missing or invalid 'files' array");
    }
    if (request.files.length > MAX_FILES) {
        emitError(
            `files[] has ${request.files.length} entries, exceeds cap ` +
            `${MAX_FILES} (set TOPO_V8_TRANSFORM_MAX_FILES to raise)`);
    }
    for (const f of request.files) {
        if (f && typeof f.content === "string" &&
            Buffer.byteLength(f.content, "utf8") > MAX_FILE_CONTENT_BYTES) {
            emitError(
                `file '${f.path ?? "?"}' content exceeds cap ` +
                `${MAX_FILE_CONTENT_BYTES} bytes ` +
                `(set TOPO_V8_TRANSFORM_MAX_FILE_CONTENT_BYTES to raise)`);
        }
    }

    if (!request.fileVisibilityMaps ||
        typeof request.fileVisibilityMaps !== "object") {
        process.stdout.write(JSON.stringify({
            success: false,
            error: "missing or invalid 'fileVisibilityMaps' object",
        }));
        process.exit(1);
    }

    const response = processRequest(request);
    process.stdout.write(JSON.stringify(response));
}

main().catch(err => {
    process.stderr.write(`error: ${err.message}\n`);
    process.exit(1);
});
