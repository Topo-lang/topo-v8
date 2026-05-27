# TopoV8Codegen — AST → source emission for the V8 family.
# Current scope: TypeScript with type annotations (behavior inherited from
# the pre-relocation TypeScriptEmitter). A mode switch for js-bare will be
# added when topo-lang-javascript lands.
#
# Relocated from topo-lang-typescript/topo-transpile/.

add_library(TopoV8Codegen STATIC
    V8Codegen.cpp
)
add_library(topo::v8::Codegen ALIAS TopoV8Codegen)

target_include_directories(TopoV8Codegen
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/Codegen>
)
# topo-core's include path comes via the PUBLIC propagation of
# topo::core::TopoTranspile — no need for the monorepo's explicit
# ${PROJECT_SOURCE_DIR}/topo-core/include PRIVATE add.
target_link_libraries(TopoV8Codegen PUBLIC topo::core::TopoTranspile)
topo_set_compiler_flags(TopoV8Codegen)

install(TARGETS TopoV8Codegen
    EXPORT topo-v8Targets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/Codegen
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/Codegen
    FILES_MATCHING PATTERN "*.h"
)
