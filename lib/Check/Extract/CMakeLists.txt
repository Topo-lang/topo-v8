# TopoV8AstExtract — AST-level check primitives for the V8 family.
#
# Relocated from topo-lang-typescript/topo-check/analysis/extract/.
#
# Current state: TS-named extractors (TypeScript prefix in class names) but
# physically located under V8 infrastructure. When topo-lang-javascript lands,
# either these generalize (mostly shared syntax) or parallel JS*Extractor
# classes sit alongside. The physical home signals shared infrastructure.

add_library(TopoV8AstExtract STATIC
    TypeScriptSymbolExtractor.cpp
    TypeScriptImportExtractor.cpp
    TypeScriptCallSiteExtractor.cpp
    TypeScriptCallEdgeExtractor.cpp
    TypeScriptSymbolAccessExtractor.cpp
)
# Canonical install-export namespace alias (must match install(EXPORT topo-v8Targets
# NAMESPACE topo::v8::)).
add_library(topo::v8::TopoV8AstExtract ALIAS TopoV8AstExtract)
add_library(topo::v8::AstExtract ALIAS TopoV8AstExtract)

target_include_directories(TopoV8AstExtract
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/Check/Extract>
)
# topo-core's include path comes via PUBLIC propagation from
# topo::core::TopoCheck — no monorepo-only PRIVATE add needed.
target_link_libraries(TopoV8AstExtract PUBLIC topo::core::TopoCheck)
topo_set_compiler_flags(TopoV8AstExtract)

install(TARGETS TopoV8AstExtract
    EXPORT topo-v8Targets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/Check/Extract
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/Check/Extract
    FILES_MATCHING PATTERN "*.h"
)
