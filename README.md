# topo-v8

V8-family backend ecosystem for Topo, sitting alongside `topo-llvm` and
`topo-jvm`. Targets V8-family host languages (TypeScript today, future
JavaScript). Zero LLVM, zero JVM dependency.

## What this package provides

Two layers:

1. **`lib/` — shared V8-family infrastructure (C++)**

   | Module | Target | Role |
   |---|---|---|
   | `lib/TsServer/` | `TopoV8TsServer` | tsserver LSP client bridge |
   | `lib/Check/Extract/` | `TopoV8AstExtract` | AST-level symbol / import / call-site / call-edge / symbol-access extractors |
   | `lib/Codegen/` | `TopoV8Codegen` | AST → host-source emitter (TypeScript-with-annotations mode) |
   | `lib/Debug/` | — | CDP adapter framework + source-map resolver |

   The TypeScript language plugin (`topo-lang-typescript`) is a thin
   shell over these. A future JavaScript plugin will reuse the same
   infrastructure.

2. **`tools/` — single-purpose Node tools for declaration enforcement**

   Each tool is an independent npm project depending on the `typescript`
   Compiler API. `topo-build-typescript` (in `topo-lang-typescript`)
   spawns each tool as a subprocess when its `[transforms.<name>].mode`
   is configured on.

   Shipped today: `tools/visibility-transform/` — strips `export`
   from symbols declared `private` in `.topo`, adds `@internal` JSDoc
   to `internal` symbols. `mode = force` fails the build when the
   transform tool or its output path is missing.

   Other passes (containment-guard, stage-assert, ...) ship one at a
   time, on real demand. No pre-built scaffolding for unstarted work.

## Build

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_PREFIX_PATH=<topo-core install prefix> \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build
ctest --test-dir build --output-on-failure
```

Downstream consumers:

```cmake
find_package(topo-v8 CONFIG REQUIRED)
target_link_libraries(<tgt> PRIVATE
    topo::v8::TopoV8TsServer
    topo::v8::TopoV8AstExtract
    topo::v8::TopoV8Codegen)
```

## Dependencies

- [`topo-core`](https://github.com/topo-lang/topo-core) — declaration
  language frontend + LSPBridge headers
- `nlohmann_json` (via vcpkg)
- Runtime: `typescript-language-server` + Node on PATH for the tsserver
  bridge to actually drive an LSP session; absent → bridge cleanly
  skips (the deep containment check downgrades to L1 with a warning).

## Status

- `lib/TsServer` / `lib/Check/Extract` / `lib/Codegen` — shipped and
  consumed by `topo-lang-typescript` end-to-end.
- `tools/visibility-transform/` — shipped; first declaration-
  enforcement pass.
- Performance passes, `@topo/runtime-*` npm packages, and FFI-boundary
  governance are deliberately open-ended. Each direction lands when a
  real use case requires it; no pre-built skeletons.

## License

MIT — see [`LICENSE`](LICENSE).
