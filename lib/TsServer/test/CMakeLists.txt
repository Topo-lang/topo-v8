# TopoV8TsServer unit tests.
#
# In-process tests for the two TsServerBridge slices that do not need a
# live `typescript-language-server`: the hover-content shape parser
# (extractHoverString) and the filesystem-fallback regex
# (findTypeDefinition when isAvailable() == false). The integration
# tests in topo-lang-typescript still cover the live-tsserver path.

add_executable(topo-v8-tsserver-tests
    TsServerBridgeTest.cpp
)
target_link_libraries(topo-v8-tsserver-tests PRIVATE
    TopoV8TsServer
    nlohmann_json::nlohmann_json
    GTest::gtest_main
)
target_include_directories(topo-v8-tsserver-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/topo-lsp/lib
)
topo_set_compiler_flags(topo-v8-tsserver-tests)
gtest_discover_tests(topo-v8-tsserver-tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain\;topo-v8\;topo-v8-tsserver" TIMEOUT 30)
