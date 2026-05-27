# TopoV8Debug — V8-family debug-side shared infrastructure.
#
# Current scope: Source Map v3 resolver used by the topo-core
# CpuProfileConverter to translate V8-reported `.js` frames back to their
# original `.ts` sources via tsc-emitted `.js.map` files. Future modules
# (CdpAdapter, BackendExtraReader) land here too.
#
# Zero LLVM dependency.

add_library(TopoV8Debug STATIC
    SourceMapResolver.cpp
)
# Canonical install-export namespace alias (must match install(EXPORT topo-v8Targets
# NAMESPACE topo::v8::)).
add_library(topo::v8::TopoV8Debug ALIAS TopoV8Debug)
add_library(topo::v8::Debug ALIAS TopoV8Debug)

target_include_directories(TopoV8Debug
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/Debug>
)
target_link_libraries(TopoV8Debug
    PUBLIC nlohmann_json::nlohmann_json
    # SourceMapResolver implements topo::profile::FrameResolver (declared
    # in topo-core's topo/Profile/FrameResolver.h). The link edge is the
    # right direction (topo-v8 → topo-core).
    PUBLIC topo::core::TopoProfile
)
topo_set_compiler_flags(TopoV8Debug)

install(TARGETS TopoV8Debug
    EXPORT topo-v8Targets
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/Debug
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/topo-v8/Debug
    FILES_MATCHING PATTERN "*.h"
)
