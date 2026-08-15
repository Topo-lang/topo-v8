# TopoV8TsServer unit tests.
#
# Two surfaces share the binary:
#
#   1. TsServerBridgeTest.cpp — in-process slices that need no live
#      `typescript-language-server`: the hover-content shape parser
#      (extractHoverString) and the filesystem-fallback regex
#      (findTypeDefinition when isAvailable() == false).
#
#   2. TsServerBridgeLiveTest.cpp — live round-trips against a real
#      `typescript-language-server` (suite TsServerBridgeLive). Each
#      case GTEST_SKIPs when the server is not installed; CI installs it
#      on every leg and converts a skip into a failure.

add_executable(topo-v8-tsserver-tests
    TsServerBridgeTest.cpp
    TsServerBridgeLiveTest.cpp
)
target_link_libraries(topo-v8-tsserver-tests PRIVATE
    TopoV8TsServer
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)
topo_set_compiler_flags(topo-v8-tsserver-tests)
# TIMEOUT 120: each live case boots its own tsserver; the per-test
# property would otherwise cap the suite at 30s under ctest --timeout 120.
gtest_discover_tests(topo-v8-tsserver-tests DISCOVERY_TIMEOUT 120
    PROPERTIES LABELS "toolchain\;topo-v8\;topo-v8-tsserver" TIMEOUT 120)
