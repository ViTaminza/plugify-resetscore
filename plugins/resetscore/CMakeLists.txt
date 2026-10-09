# resetscore — Plugify C++ plugin: port of the Pisex Metamod:Source plugin "ResetScore"
# ("mm_rs"/"sm_rs"/"rs"/"!rs" resets the caller's score, MVPs and match stats).
#
#   plugify-resetscore  the plugin shared library (entry "bin/resetscore")

set(RESETSCORE_VERSION "1.0.0")

add_library(plugify-resetscore SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(plugify-resetscore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg/pisexcompat headers must not fail -Werror on our
# sources.
target_include_directories(plugify-resetscore SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include")
target_link_libraries(plugify-resetscore PRIVATE
        pisexcompat
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-resetscore PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-resetscore PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="resetscore"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-resetscore PROPERTIES OUTPUT_NAME resetscore)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-resetscore PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resetscore.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/resetscore.pplugin"
)

# --- tests --------------------------------------------------------------------
# resetscore_tests: the live phrases file (language key + ru/en resolution) and the schema-write
# plan against the original's field list. No engine needed (the s2sdk bindings stay null).
add_executable(resetscore_tests tests/resetscore_tests.cpp)
target_include_directories(resetscore_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(resetscore_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(resetscore_tests PRIVATE pisexcompat s2sdk_bindings -static-libstdc++ -static-libgcc)
target_compile_options(resetscore_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(resetscore_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        RS_TEST_PHRASES_DIR="${CMAKE_SOURCE_DIR}/ref/prod-27016/addons/translations")
set_target_properties(resetscore_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
