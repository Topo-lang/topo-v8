# TopoV8Debug unit tests.

add_executable(topo-v8-debug-tests
    SourceMapResolverTest.cpp
)
target_link_libraries(topo-v8-debug-tests PRIVATE
    TopoV8Debug
    GTest::gtest_main
)
topo_set_compiler_flags(topo-v8-debug-tests)
gtest_discover_tests(topo-v8-debug-tests DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "toolchain;topo-v8;topo-v8-debug" TIMEOUT 30)
