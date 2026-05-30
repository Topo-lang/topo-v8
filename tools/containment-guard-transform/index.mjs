// topo-v8/tools/containment-guard-transform — ContainmentGuardPass production tool.
//
// Reads a JSON request from stdin (or argv[1]), injects runtime guards into
// the *bodies* of named non-external functions to trap use of restricted APIs
// (`eval`, `new Function`, dynamic `import()`, `Reflect.*`), and writes a JSON
// response to stdout.
//
// Protocol:
//   stdin  → {
//              "files": [{"path":"src/app.ts","content":"..."}],
//              "fileGuardTargets": {
//                "src/app.ts": ["helper", "internalUtil"]   // simple names
//              }
//            }
//   stdout → {"success":true,"results":[{"path":"src/app.ts","transformed":true,
//              "outputContent":"...","changes":["guarded helper: eval"]}]}
//
// Each guard target list is file-scoped — only top-level functions of the
// listed file whose simple names appear in the list are guarded. This
// mirrors VisibilityPass's per-file map convention.
//
// Design choice: AST call-site replacement (no prelude / no global patching).
// --------------------------------------------------------------------------
// Within each guarded function body we walk the AST and replace the
// references that the static catalog flags as escape-level:
//   eval(x)                  →  (() => { throw new Error("Topo containment
//                                violation: 'eval' is restricted ..."); })()
//   new Function(...)        →  same throwing IIFE
//   Function(...)            →  same throwing IIFE
//   import(x)                →  same throwing IIFE
//   Reflect.xxx (any use)    →  same throwing IIFE in the Reflect-spot
//
// This was chosen over the alternative ("shadow restricted globals with
// `const` bindings at the top of the body") because `eval` and `arguments`
// are special binding identifiers that the ECMAScript strict-mode spec
// rejects as `const` declarees. TypeScript's parseDiagnostics is permissive
// on this (the error fires in semantic checking), but Node/V8 rejects the
// shadowed module at load with `SyntaxError: Unexpected eval or arguments
// in strict mode`. AST replacement sidesteps the strict-mode trap entirely
// and is also surface-narrower — only flagged constructs change, the rest
// of the body is byte-identical.
//
// Trade-offs accepted:
//   - We do not trap *indirect* aliases of the restricted globals
//     (`const f = eval; f("x")` would not be caught at the second line).
//     ContainmentCheck already catches the direct alias assignment at
//     analysis time; the runtime guard's job is to enforce the direct
//     surface, not to prove the absence of all aliases.
//   - We trap only references *lexically inside* the guarded function
//     body. Nested function/class declarations have their own .topo
//     declarations and get their own guards (or external-boundary
//     delegation). The visitor does NOT descend into nested function-
//     like scopes within the guarded body.
//   - Post-rewrite parse validation via `ts.createSourceFile.parseDiagnostics`
//     catches structural breakage before we ship the transformed text.
//     Same gate as VisibilityPass.

import ts from "typescript";

// ---------------------------------------------------------------------------
// Restricted-API set — mirrors TypeScriptUnsafeCatalog Level 4 (Escape).
// First-batch coverage; Atomics / Proxy / child_process / vm are present
// in the static catalog but deferred to follow-up extensions of this pass.
// ---------------------------------------------------------------------------

const RESTRICTED_CALLEES = new Set(["eval", "Function"]);
const RESTRICTED_MEMBERS = new Set(["Reflect"]);

// ---------------------------------------------------------------------------
// Trap-expression builder. Produces  (() => { throw new Error(<message>); })()
// ---------------------------------------------------------------------------

function buildThrowIife(factory, message) {
    const arrow = factory.createArrowFunction(
        /*modifiers*/ undefined,
        /*typeParameters*/ undefined,
        /*parameters*/ [],
        /*type*/ undefined,
        factory.createToken(ts.SyntaxKind.EqualsGreaterThanToken),
        factory.createBlock([
            factory.createThrowStatement(
                factory.createNewExpression(
                    factory.createIdentifier("Error"),
                    /*typeArguments*/ undefined,
                    [factory.createStringLiteral(message)])),
        ], /*multiLine*/ true));
    return factory.createCallExpression(
        factory.createParenthesizedExpression(arrow),
        /*typeArguments*/ undefined,
        /*arguments*/ []);
}

// ---------------------------------------------------------------------------
// Scope-binding extraction. Given a node that introduces a new lexical
// scope (Block, ForStatement init scope, CatchClause, etc.), collect the
// identifier names bound *directly* in that scope so the visitor can
// skip rewriting references that resolve to a user-defined shadow.
//
// We bind the names eagerly on scope entry (rather than incrementally as
// the visitor walks past each declaration) because:
//
//   1. Function declarations and `var` are hoisted — a reference earlier
//      in the block legitimately resolves to a later-declared name.
//   2. `let` / `const` are TDZ-locked at the binding scope's top, but
//      treating a reference inside the dead-zone as "shadowed" is still
//      semantically correct for *containment* purposes (the user wrote
//      a binding, so it isn't a reference to the restricted global).
//
// Collection is shallow — it does NOT descend into nested function-like
// scopes, nested blocks, etc. The visitor pushes a new scope set when it
// enters each of those.
// ---------------------------------------------------------------------------

function collectBindingNamesShallow(container) {
    const names = new Set();

    const addBindingName = (name) => {
        if (!name) return;
        if (ts.isIdentifier(name)) {
            names.add(name.text);
            return;
        }
        if (ts.isObjectBindingPattern(name) || ts.isArrayBindingPattern(name)) {
            for (const element of name.elements) {
                if (ts.isBindingElement(element)) {
                    addBindingName(element.name);
                }
            }
        }
    };

    const visitStatement = (stmt) => {
        if (ts.isVariableStatement(stmt)) {
            for (const d of stmt.declarationList.declarations) {
                addBindingName(d.name);
            }
        } else if (ts.isFunctionDeclaration(stmt) && stmt.name) {
            names.add(stmt.name.text);
        } else if (ts.isClassDeclaration(stmt) && stmt.name) {
            names.add(stmt.name.text);
        } else if (ts.isImportDeclaration(stmt) && stmt.importClause) {
            const clause = stmt.importClause;
            if (clause.name) names.add(clause.name.text);
            if (clause.namedBindings) {
                if (ts.isNamespaceImport(clause.namedBindings)) {
                    names.add(clause.namedBindings.name.text);
                } else if (ts.isNamedImports(clause.namedBindings)) {
                    for (const spec of clause.namedBindings.elements) {
                        names.add(spec.name.text);
                    }
                }
            }
        }
    };

    if (ts.isBlock(container) || ts.isSourceFile(container)) {
        for (const stmt of container.statements) visitStatement(stmt);
    } else if (ts.isForStatement(container)) {
        // `for (let i = 0; ...; ...)` — the initializer scope-binds i.
        if (container.initializer &&
            ts.isVariableDeclarationList(container.initializer)) {
            for (const d of container.initializer.declarations) {
                addBindingName(d.name);
            }
        }
    } else if (ts.isForInStatement(container) ||
               ts.isForOfStatement(container)) {
        if (container.initializer &&
            ts.isVariableDeclarationList(container.initializer)) {
            for (const d of container.initializer.declarations) {
                addBindingName(d.name);
            }
        }
    } else if (ts.isCatchClause(container)) {
        if (container.variableDeclaration) {
            addBindingName(container.variableDeclaration.name);
        }
    }

    return names;
}

// Returns true if any active scope on the stack declares `name`. Used to
// short-circuit the rewrite when the user has shadowed a restricted
// global (e.g. `const Reflect = obj`).
function isShadowed(scopeStack, name) {
    for (const scope of scopeStack) {
        if (scope.has(name)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Per-function-body rewrite. Returns the new body block plus a list of
// `changes` strings describing each substitution.
// ---------------------------------------------------------------------------

function rewriteBody(context, body, fnName, parameters, changes) {
    const factory = context.factory;

    const trapFor = (api) =>
        buildThrowIife(
            factory,
            `Topo containment violation: '${api}' is restricted in ` +
            `non-external function '${fnName}'`);

    // Stack of active lexical-scope binding sets. The outermost frame
    // covers the guarded function body itself (parameters + top-level
    // declarations); nested frames cover Blocks, for-init scopes, and
    // catch-clauses encountered during the walk.
    const scopeStack = [];

    // Seed the top-level frame with the guarded function's parameters
    // (declared by the enclosing FunctionDeclaration, so a binding name
    // like `Reflect` in the param list shadows the global throughout the
    // body) plus the body-block's directly-declared names.
    const bodyScope = collectBindingNamesShallow(body);
    for (const p of parameters ?? []) {
        if (p.name && ts.isIdentifier(p.name)) {
            bodyScope.add(p.name.text);
        } else if (p.name) {
            // Destructured parameter: collect the bound identifiers.
            const collectFromPattern = (pattern) => {
                if (ts.isIdentifier(pattern)) {
                    bodyScope.add(pattern.text);
                    return;
                }
                if (ts.isObjectBindingPattern(pattern) ||
                    ts.isArrayBindingPattern(pattern)) {
                    for (const el of pattern.elements) {
                        if (ts.isBindingElement(el)) {
                            collectFromPattern(el.name);
                        }
                    }
                }
            };
            collectFromPattern(p.name);
        }
    }
    scopeStack.push(bodyScope);

    const visit = (node) => {
        // Do NOT descend into nested function-like scopes. Their bodies have
        // their own .topo declarations and their own guard decisions; double-
        // rewriting would compound errors (and might shadow a legitimate
        // external-boundary delegation).
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

        // Push/pop a scope frame around any node that introduces a new
        // lexical scope. The frame is built shallowly so it sees only
        // names bound *at* this scope; nested scopes get their own frame.
        const introducesScope =
            (ts.isBlock(node) && node !== body) ||
            ts.isForStatement(node) ||
            ts.isForInStatement(node) ||
            ts.isForOfStatement(node) ||
            ts.isCatchClause(node);
        if (introducesScope) {
            scopeStack.push(collectBindingNamesShallow(node));
            try {
                return ts.visitEachChild(node, visit, context);
            } finally {
                scopeStack.pop();
            }
        }

        // Dynamic import(): expression is the ImportKeyword. `import` is
        // a reserved word, so no shadowing concern.
        if (ts.isCallExpression(node) &&
            node.expression.kind === ts.SyntaxKind.ImportKeyword) {
            changes.push(`guarded ${fnName}: dynamic import()`);
            return trapFor("import()");
        }

        // Call expressions: eval(...) / Function(...).
        if (ts.isCallExpression(node) &&
            ts.isIdentifier(node.expression) &&
            RESTRICTED_CALLEES.has(node.expression.text) &&
            !isShadowed(scopeStack, node.expression.text)) {
            const name = node.expression.text;
            changes.push(`guarded ${fnName}: ${name}()`);
            return trapFor(name);
        }

        // new Function(...) — NewExpression whose expression is `Function`.
        if (ts.isNewExpression(node) &&
            ts.isIdentifier(node.expression) &&
            RESTRICTED_CALLEES.has(node.expression.text) &&
            !isShadowed(scopeStack, node.expression.text)) {
            const name = node.expression.text;
            changes.push(`guarded ${fnName}: new ${name}()`);
            return trapFor(`new ${name}`);
        }

        // Reflect — replace the `Reflect` identifier wherever it appears
        // as a Reference (PropertyAccess: Reflect.ownKeys, ElementAccess:
        // Reflect["get"], or bare expression). Skip identifiers that are
        // declaration names (parameter, variable, member name): they are
        // not references and shadowing them is a separate concern.
        if (ts.isIdentifier(node) &&
            RESTRICTED_MEMBERS.has(node.text)) {
            const parent = node.parent;
            const isNamePosition =
                parent && (
                    (ts.isVariableDeclaration(parent) && parent.name === node) ||
                    (ts.isParameter(parent) && parent.name === node) ||
                    (ts.isBindingElement(parent) && parent.name === node) ||
                    (ts.isPropertyAssignment(parent) && parent.name === node) ||
                    (ts.isPropertyDeclaration(parent) && parent.name === node) ||
                    (ts.isMethodDeclaration(parent) && parent.name === node) ||
                    (ts.isFunctionDeclaration(parent) && parent.name === node) ||
                    (ts.isClassDeclaration(parent) && parent.name === node) ||
                    (ts.isLabeledStatement(parent) && parent.label === node) ||
                    (ts.isImportSpecifier(parent)) ||
                    (ts.isExportSpecifier(parent)) ||
                    (ts.isPropertyAccessExpression(parent) && parent.name === node)
                );
            if (!isNamePosition && !isShadowed(scopeStack, node.text)) {
                changes.push(`guarded ${fnName}: ${node.text}`);
                return trapFor(node.text);
            }
        }

        return ts.visitEachChild(node, visit, context);
    };

    return ts.visitEachChild(body, visit, context);
}

// ---------------------------------------------------------------------------
// File-level rewrite. Visits SourceFile children; for each FunctionDeclaration
// whose simple name is in `guardNames`, runs rewriteBody on its body.
// ---------------------------------------------------------------------------

function rewrite(sourceText, guardNames, fileName = "input.ts") {
    const source = ts.createSourceFile(
        fileName, sourceText, ts.ScriptTarget.ES2022,
        /*setParentNodes*/ true);
    const guardSet = new Set(guardNames);
    const changes = [];

    const transformer = (context) => (root) => {
        const factory = context.factory;
        const visit = (node) => {
            if (node.parent &&
                node.parent.kind === ts.SyntaxKind.SourceFile &&
                ts.isFunctionDeclaration(node) &&
                node.name &&
                guardSet.has(node.name.text) &&
                node.body) {
                const fnName = node.name.text;
                const newBody = rewriteBody(
                    context, node.body, fnName, node.parameters, changes);
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
    const { files, fileGuardTargets } = request;
    const results = [];

    for (const file of files) {
        try {
            const targets = fileGuardTargets[file.path] ?? [];
            if (targets.length === 0) {
                // Fast-path: nothing to guard in this file. Avoid printer
                // reflow that would spuriously mark it "changed".
                results.push({
                    path: file.path,
                    transformed: false,
                    outputContent: file.content,
                    changes: [],
                });
                continue;
            }
            const { output, changes } = rewrite(
                file.content, targets, file.path);
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

    if (!request.fileGuardTargets ||
        typeof request.fileGuardTargets !== "object") {
        process.stdout.write(JSON.stringify({
            success: false,
            error: "missing or invalid 'fileGuardTargets' object",
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
