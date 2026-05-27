# VisibilityPass — AST-level prototype (historical, npm-only)

> **Status**: Superseded for production use by
> [`topo-v8/tools/visibility-transform/`](../../tools/visibility-transform/),
> which is wired into `topo-build-typescript` via Topo.toml's
> `[transforms.visibility].mode`. This prototype directory is kept as a
> minimal npm-only / Node-only teaching sample of the AST-rewrite path.
> It is NOT in ctest, NOT invoked by `topo-build`, and changes here have
> no effect on production builds.
>
> Historical origin: this artifact was created as an early
> proof-of-feasibility that the `typescript` Compiler API can drive
> AST-level declaration-enforcement rewrites. That feasibility evidence
> is unchanged; the production implementation has since taken over.

## What this is

A minimal Node prototype that proves a declaration-enforcement Pass can
be implemented as a pure TypeScript-AST rewrite, using the official
`typescript` Compiler API. **Not** production scaffolding — the
production version lives at `topo-v8/tools/visibility-transform/`.

## What it does

Given a `.topo`-derived visibility map (JSON: `symbol -> "public" | "internal" | "private"`)
and a `.ts` source file, the prototype rewrites top-level declaration
visibility to match:

| Declared | Action on `.ts` declaration |
|----------|-----------------------------|
| `public` | keep `export`, no JSDoc tag |
| `internal` | keep `export`, prepend `/** @internal */` JSDoc |
| `private` | strip `export` keyword |

Anything not listed in the visibility map is left unchanged. This mirrors
the intended behavior: rewrite `export` according to `.topo`, add
`@internal` JSDoc, and remove the export of undeclared symbols.

## What this proves

1. The `typescript` Compiler API can drive AST-level rewrites without depending
   on tsc's program / type-checker phases — `ts.createSourceFile` +
   `ts.transform` + `ts.createPrinter` is enough for declaration-level edits.
2. Adding/removing modifier keywords and prepending JSDoc both compose with
   the same printer output — emitted source compiles back through tsc.
3. The rewrite is deterministic on the inputs (no embedded state, no time-of-
   compile dependency), so it can later be wired into a `topo-build-v8-typescript`
   backend tool.

## What this is NOT

- The production VisibilityPass. That lives in
  [`topo-v8/tools/visibility-transform/`](../../tools/visibility-transform/)
  and adds: file-scoped visibility maps, error propagation through the
  topo-build pipeline, post-rewrite syntactic re-parse, and integration
  with `[transforms.visibility].mode`. This prototype has none of those.
- Coverage of any other declaration-enforcement pass:
  ContainmentGuardPass / StageAssertPass / Ffi*Pass are out of scope for
  this prototype. Those are added incrementally in production as needed.

## Run

```sh
cd topo-v8/prototypes/visibility-pass
npm install            # pulls typescript ^5.4
node --test test.mjs   # runs the round-trip fixture test
```

The `test.mjs` script asserts the rewritten output against
`fixtures/sample.expected.ts` for the three visibility-action paths
(`public`, `internal`, `private`).
