# TopoV8CompilerFlags.cmake — standalone compiler-flag helper for topo-v8.
# Same shape as topo-core / topo-lang variants.

if(NOT WIN32)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(APPLE)
        set(CMAKE_MACOSX_RPATH ON)
    endif()
endif()

set(TOPO_V8_SANITIZER "" CACHE STRING
    "Enable sanitizers (address, undefined, thread, memory)")

if(TOPO_V8_SANITIZER)
    message(STATUS "topo-v8 sanitizers enabled: ${TOPO_V8_SANITIZER}")
endif()

function(topo_v8_apply_sanitizer target)
    if(NOT TOPO_V8_SANITIZER)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target}
            PRIVATE -fsanitize=${TOPO_V8_SANITIZER} -fno-omit-frame-pointer)
        target_link_options(${target}
            PRIVATE -fsanitize=${TOPO_V8_SANITIZER})
    endif()
endfunction()

function(topo_set_compiler_flags target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()

    topo_v8_apply_sanitizer(${target})
endfunction()

# Stub for monorepo PCH helper that some lib CMakeLists may call.
function(topo_apply_std_pch target)
endfunction()
