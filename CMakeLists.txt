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
if(NOT TARGET topo::core::TopoBasic)
    find_package(topo-core CONFIG REQUIRED)
endif()

# ── External dependencies ─────────────────────────────
find_package(nlohmann_json CONFIG REQUIRED)

# ── Library layer ─────────────────────────────────────
add_subdirectory(lib)

# ── Tests (self-gated) ────────────────────────────────
# The lib/TsServer/test (15 cases) and lib/Debug/test (7 cases) suites used to
# run ONLY in the meta full-stack build (via the meta-root's explicit
# add_subdirectory) — the per-repo public CI built topo-v8 with no tests and
# passed green ("No tests were found!!!"). This self-gate lets the standalone
# CI run the 22 cases. It stays DORMANT in the meta build (TOPO_V8_BUILD_TESTS
# defaults OFF there, so the meta's explicit adds remain the single source —
# no double-add).
option(TOPO_V8_BUILD_TESTS "Build topo-v8 tests" OFF)
if(TOPO_V8_BUILD_TESTS AND (PROJECT_IS_TOP_LEVEL OR TOPO_META_DRIVING_TESTS))
    enable_testing()
    # Meta-embedded mode reuses the meta's googletest; only fetch standalone.
    if(NOT TARGET GTest::gtest_main)
        include(FetchContent)
        FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
    set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)
    include(GoogleTest)
    add_subdirectory(lib/TsServer/test)
    add_subdirectory(lib/Debug/test)
endif()

# Topov8Placeholder — historically a no-op INTERFACE library for
# structural dependency edges from language plugins. Now re-exports
# the real V8 infrastructure libraries.
add_library(Topov8Placeholder INTERFACE)
# Canonical install-export namespace alias (must match install(EXPORT topo-v8Targets
# NAMESPACE topo::v8::) — this is the form downstream find_package sees and
# the sentinel target the meta-repo's dual-mode guard checks for).
add_library(topo::v8::Topov8Placeholder ALIAS Topov8Placeholder)
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
