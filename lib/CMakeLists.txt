# topo-v8/lib/ — V8-family shared AST infrastructure.
#
# Each subdirectory defines a library target consumed by language plugins:
#   TsServer/  → TopoV8TsServer     (tsserver client + AST query)
#   Check/     → TopoV8AstExtract + TopoV8UnsafeCatalog (AST-level check primitives)
#   Codegen/   → TopoV8Codegen      (AST → source emission, ts-with-annotations / js-bare)
#
# Language plugins (topo-lang-typescript, future topo-lang-javascript) link
# these directly or via the Topov8Placeholder alias in topo-v8/CMakeLists.txt.

add_subdirectory(TsServer)
add_subdirectory(Check)
add_subdirectory(Codegen)
add_subdirectory(Debug)
