# TopoV8TsServer — tsserver client + AST query abstraction.
# Relocated from topo-lang-typescript/topo-lsp/.
#
# Wraps `typescript-language-server --stdio`. Serves as the shared tsserver
# client for the V8 language family (TypeScript today; JavaScript when
# topo-lang-javascript lands — tsserver supports JS natively via allowJs).

add_library(TopoV8TsServer STATIC
    TsServerBridge.cpp
)
add_library(topo::v8::TsServer ALIAS TopoV8TsServer)

target_include_directories(TopoV8TsServer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/TsServer>
)
target_link_libraries(TopoV8TsServer PUBLIC topo::core::TopoLSPBridge)
topo_set_compiler_flags(TopoV8TsServer)

install(TARGETS TopoV8TsServer
    EXPORT topo-v8Targets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/TsServer
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/TsServer
    FILES_MATCHING PATTERN "*.h"
)
