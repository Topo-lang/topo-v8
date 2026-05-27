cmake_minimum_required(VERSION 3.20)

project(topo-v8
    VERSION 0.1.0
    DESCRIPTION "V8-family shared infrastructure (TsServer + AST extract + codegen + debug)"
    HOMEPAGE_URL "https://github.com/topo-lang/topo-v8"
    LANGUAGES CXX
)

# topo-v8 — V8-family shared AST infrastructure (lib/) + declaration-
# enforcement tools (tools/).
#
# Two layers live here:
#
# 1) lib/   — V8-family shared AST layer (load-bearing today).
#    The tsserver client, AST check primitives, and source codegen that
#    TypeScript (and a future JavaScript) language plugins share. Each
#    per-language plugin injects this layer as a thin shell.
#
# 2) tools/ — Single-pass declaration-enforcement Node tools.
#    Each tool is an independent npm project, spawned as a subprocess
#    by topo-build-typescript when its `[transforms.<name>]` knob is on.
#
# Other contents:
#   - prototypes/visibility-pass/  historical PoC (npm-only, NOT built by CMake)
#   - README.md                    current status + extension-direction index
#   - topo-backend.json            machine-readable identity + landed-tool registry

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(TopoV8CompilerFlags)

# ── Upstream Topo packages ────────────────────────────
# topo-core exports topo::core::TopoLSPBridge, TopoCheck, TopoTranspile,
# TopoProfile — the PUBLIC dependency closure of topo-v8's libs.
# (TopoTranspile in topo-core is gated on topo-lang being available; if
# the upstream topo-core was built without topo-lang on the prefix path,
# TopoTranspile is absent and topo-v8's Codegen lib will fail to link.)
find_package(topo-core CONFIG REQUIRED)

# ── External dependencies ─────────────────────────────
find_package(nlohmann_json CONFIG REQUIRED)

# ── Library layer ─────────────────────────────────────
add_subdirectory(lib)

# Topov8Placeholder — historically a no-op INTERFACE library for
# structural dependency edges from language plugins. Now re-exports
# the real V8 infrastructure libraries.
add_library(Topov8Placeholder INTERFACE)
add_library(topo::v8::Placeholder ALIAS Topov8Placeholder)
target_link_libraries(Topov8Placeholder INTERFACE
    TopoV8TsServer
    TopoV8AstExtract
    TopoV8Codegen
)
target_compile_definitions(Topov8Placeholder INTERFACE TOPO_V8_PLACEHOLDER=0)

install(TARGETS Topov8Placeholder
    EXPORT topo-v8Targets
)

# ── Install / export ──────────────────────────────────
install(EXPORT topo-v8Targets
    FILE topo-v8Targets.cmake
    NAMESPACE topo::v8::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-v8
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/topo-v8Config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/topo-v8Config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-v8
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/topo-v8ConfigVersion.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/topo-v8Config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/topo-v8ConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/topo-v8
)
