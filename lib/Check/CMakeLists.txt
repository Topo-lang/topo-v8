# topo-v8/lib/Check — AST-level check primitives for the V8 family.
#
# Boundary: the shared infrastructure is the generic AST
# extractors (Extract/). TS-specific catalog (safe patterns / unsafe catalog /
# safety analyzer) stays in topo-lang-typescript/topo-check/analysis/catalog/
# because its code is TS-syntax-specific and pattern data is TS-centric — no
# generic V8 framework to factor out today. When topo-lang-javascript is
# added and actual shared catalog needs emerge, a Catalog/ subdirectory lands
# here.

add_subdirectory(Extract)
