// topo-v8/tools/stage-assert-transform — StageAssertPass production tool.
//
// Reads a JSON request from stdin (or argv[1]), injects runtime ordering
// assertions into named host functions whose .topo declaration carries
// `stage<N>` operations, and writes a JSON response to stdout.
//
// Protocol:
//   stdin  → {
//              "files": [{"path":"src/app.ts","content":"..."}],
//              "fileStageMaps": {
//                "src/app.ts": {
//                  "pipeline": [
//                    ["fetch", "parse"],   // stage 1 callees
//                    ["transform"],         // stage 2 callees
//                    ["emit"]              // stage 3 callees
//                  ]
//                }
//              }
//            }
//   stdout → {"success":true,"results":[{"path":"src/app.ts","transformed":true,
//              "outputContent":"...","changes":["stage-asserted pipeline: fetch@1"]}]}
//
// Each stage map is file-scoped — only top-level functions whose simple names
// appear as a key are rewritten. The value is an array indexed by stage-1
// (i.e. stageMap[0] is the list of stage<1> callees, stageMap[1] is stage<2>,
// …). The bridge in topo-build-typescript builds this from .topo logicBlocks
// (parallel arrays calledFunctions[] + stages[]).
//
// Rewrite strategy
// ----------------
// At the top of each mapped function's body we inject:
//     let __topoStage = 0;
// For each direct call expression  fn(args)  inside the body whose callee
// simple-name is in any stageMap[i], we replace the call with a 0-arg IIFE
// that:
//   1. captures the target stage N (i+1)
//   2. throws if N < __topoStage (monotonicity violation)
//   3. updates __topoStage = max(__topoStage, N)
//   4. returns the original call expression
// Concretely:
//     fetch(x)
// becomes:
//     (() => {
//         const __need = 1;
//         if (__need < __topoStage) {
//             throw new Error("Topo stage assertion: 'fetch' declared " +
//                 "stage<1> but counter is at stage<" + __topoStage + ">");
//         }
//         __topoStage = Math.max(__topoStage, __need);
//         return fetch(x);
//     })()
//
// Notes / trade-offs:
//   - The wrapper preserves arguments by lexical capture — argument expressions
//     are evaluated as part of the inner call, NOT lifted out. Their side
//     effects fire in original textual position relative to the assertion.
//   - We do NOT descend into nested function-like scopes. A callsite inside
//     a nested closure has a separate lifetime and its own stage discipline
//     (or none). Nested .topo fns get their own pass invocations.
//   - If a callee appears in multiple stage buckets (shouldn't happen for
//     well-formed .topo, but defensive), we pick the highest stage so the
//     monotonicity invariant remains sound.
//   - Method calls (`obj.fetch(...)`) are NOT rewritten — stage tracking is
//     a simple-name contract and matching `.method()` would over-match.
//   - Post-rewrite parseDiagnostics validation is required.

import ts from "typescript";
import { createHash } from "node:crypto";

// Default injected identifiers — used when the host function body does
// not bind either name. Collisions trigger a deterministic hashed form
// (see chooseInjectedIdentifiers).
const DEFAULT_COUNTER_NAME = "__topoStage";
const DEFAULT_NEED_NAME = "__need";

// ---------------------------------------------------------------------------
// Collision detection: scan a function declaration (parameter list + body)
// for any binding/identifier whose simple name equals the candidate name.
// Returns true on first hit.
//
// Walking the function node — not just its body — is required because
// parameters live as children of the function node (`fnNode.parameters`),
// not of the body block. A function that declares `__topoStage` as a
// parameter shares the body's lexical scope; injecting `let __topoStage`
// into the body would redeclare it (SyntaxError in strict mode).
//
// We walk Identifier nodes inside parameters and inside the body — covers
// parameter names (including destructuring), var/let/const declarations,
// nested function declarations, etc. This is over-approximate but cheap
// and avoids a real scope analysis.
// ---------------------------------------------------------------------------

function functionHasIdentifier(fnNode, name) {
    let found = false;
    const visit = (node) => {
        if (found) return;
        if (ts.isIdentifier(node) && node.text === name) {
            found = true;
            return;
        }
        ts.forEachChild(node, visit);
    };
    // Walk parameter list first (cheaper, and the bug we fix).
    for (const param of fnNode.parameters) {
        visit(param);
        if (found) return true;
    }
    // Then walk the body (statements + nested decls).
    if (fnNode.body) {
        ts.forEachChild(fnNode.body, visit);
    }
    return found;
}

// ---------------------------------------------------------------------------
// Pick injected identifier names for a function. If neither default name
// collides with a parameter or body identifier, return the defaults.
// Otherwise, derive a deterministic hash-suffixed form from the function
// name so the renamed identifier is stable across runs (same function →
// same suffix) and unlikely to collide with user code unless the user is
// following the exact same hash convention.
// ---------------------------------------------------------------------------

function chooseInjectedIdentifiers(fnNode, fnName) {
    const counterClash = functionHasIdentifier(fnNode, DEFAULT_COUNTER_NAME);
    const needClash = functionHasIdentifier(fnNode, DEFAULT_NEED_NAME);
    if (!counterClash && !needClash) {
        return {
            counter: DEFAULT_COUNTER_NAME,
            need: DEFAULT_NEED_NAME,
            collisionAvoided: false,
        };
    }
    // Hash on fnName so the suffix is deterministic for a given function.
    const suffix = createHash("sha256")
        .update(fnName)
        .digest("hex")
        .slice(0, 8);
    return {
        counter: `__topoStage$${suffix}`,
        need: `__need$${suffix}`,
        collisionAvoided: true,
    };
}

// ---------------------------------------------------------------------------
// Build a "guard-wrapped call" expression around an existing CallExpression.
// ---------------------------------------------------------------------------

function buildAssertedCall(factory, originalCall, fnName, calleeName, stage,
                            injectedNames) {
    const needId = factory.createIdentifier(injectedNames.need);
    const counterId = factory.createIdentifier(injectedNames.counter);

    // const __need = <stage>;
    const declNeed = factory.createVariableStatement(
        /*modifiers*/ undefined,
        factory.createVariableDeclarationList(
            [factory.createVariableDeclaration(
                needId,
                /*exclamationToken*/ undefined,
                /*type*/ undefined,
                factory.createNumericLiteral(String(stage)))],
            ts.NodeFlags.Const));

    // if (__need < __topoStage) { throw new Error("..."); }
    const message =
        `Topo stage assertion: '${calleeName}' declared stage<${stage}> ` +
        `but counter is at stage<`;
    const throwStmt = factory.createThrowStatement(
        factory.createNewExpression(
            factory.createIdentifier("Error"),
            /*typeArguments*/ undefined,
            [factory.createBinaryExpression(
                factory.createStringLiteral(message),
                factory.createToken(ts.SyntaxKind.PlusToken),
                factory.createBinaryExpression(
                    counterId,
                    factory.createToken(ts.SyntaxKind.PlusToken),
                    factory.createStringLiteral(">")))]));
    const ifStmt = factory.createIfStatement(
        factory.createBinaryExpression(
            needId,
            factory.createToken(ts.SyntaxKind.LessThanToken),
            counterId),
        factory.createBlock([throwStmt], /*multiLine*/ true));

    // __topoStage = Math.max(__topoStage, __need);
    const updateStmt = factory.createExpressionStatement(
        factory.createAssignment(
            counterId,
            factory.createCallExpression(
                factory.createPropertyAccessExpression(
                    factory.createIdentifier("Math"),
                    factory.createIdentifier("max")),
                /*typeArguments*/ undefined,
                [counterId, needId])));

    // return <originalCall>;
    const returnStmt = factory.createReturnStatement(originalCall);

    const arrow = factory.createArrowFunction(
        /*modifiers*/ undefined,
        /*typeParameters*/ undefined,
        /*parameters*/ [],
        /*type*/ undefined,
        factory.createToken(ts.SyntaxKind.EqualsGreaterThanToken),
        factory.createBlock(
            [declNeed, ifStmt, updateStmt, returnStmt], /*multiLine*/ true));

    return factory.createCallExpression(
        factory.createParenthesizedExpression(arrow),
        /*typeArguments*/ undefined,
        /*arguments*/ []);
}

// ---------------------------------------------------------------------------
// Per-function-body rewrite. Returns the new body block plus a list of
// `changes` strings describing each substitution.
// ---------------------------------------------------------------------------

function rewriteBody(context, fnNode, fnName, calleeToStage, changes) {
    const factory = context.factory;
    const body = fnNode.body;
    const injectedNames = chooseInjectedIdentifiers(fnNode, fnName);
    if (injectedNames.collisionAvoided) {
        changes.push(
            `stage-assert: function '${fnName}' binds '__topoStage' or ` +
            `'__need' — using hashed form '${injectedNames.counter}' / ` +
            `'${injectedNames.need}' to avoid collision`);
    }

    const visit = (node) => {
        // Stop at nested function-like scopes — their own stage discipline
        // (if any) is handled by their own pass invocation.
        if (
            ts.isFunctionDeclaration(node) ||
            ts.isFunctionExpression(node) ||
            ts.isArrowFunction(node) ||
            ts.isMethodDeclaration(node) ||
            ts.isClassDeclaration(node) ||
            ts.isClassExpression(node) ||
            ts.isGetAccessorDeclaration(node) ||
            ts.isSetAccessorDeclaration(node) ||
            ts.isConstructorDeclaration(node)
        ) {
            return node;
        }

        // Direct identifier call:  fetch(args)
        if (ts.isCallExpression(node) &&
            ts.isIdentifier(node.expression) &&
            calleeToStage.has(node.expression.text)) {
            const calleeName = node.expression.text;
            const stage = calleeToStage.get(calleeName);
            // Recurse into arguments first so nested mapped calls also get
            // wrapped (children processed before we replace the parent).
            const updatedArgs = node.arguments.map(arg =>
                ts.visitNode(arg, visit));
            const updatedCall = factory.updateCallExpression(
                node, node.expression, node.typeArguments, updatedArgs);
            changes.push(
                `stage-asserted ${fnName}: ${calleeName}@${stage}`);
            return buildAssertedCall(
                factory, updatedCall, fnName, calleeName, stage, injectedNames);
        }

        return ts.visitEachChild(node, visit, context);
    };

    // Walk the body's statements with our visitor.
    const newBody = ts.visitEachChild(body, visit, context);

    // Inject  let <counter> = 0;  at the very top. The counter name is
    // the hashed form when the user body already binds __topoStage; see
    // chooseInjectedIdentifiers. Using a fixed name here would silently
    // collide with a user binding of the same identifier.
    const prologue = factory.createVariableStatement(
        /*modifiers*/ undefined,
        factory.createVariableDeclarationList(
            [factory.createVariableDeclaration(
                factory.createIdentifier(injectedNames.counter),
                /*exclamationToken*/ undefined,
                /*type*/ undefined,
                factory.createNumericLiteral("0"))],
            ts.NodeFlags.Let));

    return factory.updateBlock(
        newBody, [prologue, ...newBody.statements]);
}

// ---------------------------------------------------------------------------
// Build a callee→stage map from a stageMap (array indexed by stage-1).
// If a name appears in multiple buckets, the highest stage wins.
// ---------------------------------------------------------------------------

function flattenStageMap(stageMap) {
    const m = new Map();
    for (let i = 0; i < stageMap.length; ++i) {
        const stage = i + 1;
        const bucket = stageMap[i] || [];
        for (const name of bucket) {
            const prior = m.get(name);
            if (prior === undefined || stage > prior) {
                m.set(name, stage);
            }
        }
    }
    return m;
}

// ---------------------------------------------------------------------------
// File-level rewrite. Visits SourceFile children; for each top-level
// FunctionDeclaration whose simple name is a key in `fnToStageMap`, runs
// rewriteBody on its body with the corresponding callee→stage map.
// ---------------------------------------------------------------------------

function rewrite(sourceText, fnToStageMap, fileName = "input.ts") {
    const source = ts.createSourceFile(
        fileName, sourceText, ts.ScriptTarget.ES2022,
        /*setParentNodes*/ true);
    const changes = [];

    const transformer = (context) => (root) => {
        const factory = context.factory;
        const visit = (node) => {
            if (node.parent &&
                node.parent.kind === ts.SyntaxKind.SourceFile &&
                ts.isFunctionDeclaration(node) &&
                node.name &&
                node.body &&
                Object.prototype.hasOwnProperty.call(
                    fnToStageMap, node.name.text)) {
                const fnName = node.name.text;
                const calleeToStage = flattenStageMap(
                    fnToStageMap[fnName] || []);
                if (calleeToStage.size === 0) return node;
                const newBody = rewriteBody(
                    context, node, fnName, calleeToStage, changes);
                return factory.updateFunctionDeclaration(
                    node, node.modifiers, node.asteriskToken, node.name,
                    node.typeParameters, node.parameters, node.type, newBody);
            }
            return ts.visitEachChild(node, visit, context);
        };
        return ts.visitEachChild(root, visit, context);
    };

    const result = ts.transform(source, [transformer]);
    const printer = ts.createPrinter({ newLine: ts.NewLineKind.LineFeed });
    const out = printer.printFile(result.transformed[0]);
    result.dispose();

    // Post-rewrite syntactic re-parse.
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
    const { files, fileStageMaps } = request;
    const results = [];

    for (const file of files) {
        try {
            const fnToStageMap = fileStageMaps[file.path] ?? {};
            if (Object.keys(fnToStageMap).length === 0) {
                // Fast-path: nothing to assert in this file.
                results.push({
                    path: file.path,
                    transformed: false,
                    outputContent: file.content,
                    changes: [],
                });
                continue;
            }
            const { output, changes } = rewrite(
                file.content, fnToStageMap, file.path);
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

// Input size caps (guard against unbounded transform input).
// See visibility-transform/index.mjs for the rationale; the caps and env
// names are deliberately shared across the three V8 transform tools so a
// user setting one env var lifts the cap on all three uniformly.
const DEFAULT_MAX_INPUT_BYTES = 64 * 1024 * 1024;
const DEFAULT_MAX_FILES = 10_000;
const DEFAULT_MAX_FILE_CONTENT_BYTES = 16 * 1024 * 1024;

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

    if (!request.fileStageMaps ||
        typeof request.fileStageMaps !== "object") {
        process.stdout.write(JSON.stringify({
            success: false,
            error: "missing or invalid 'fileStageMaps' object",
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
