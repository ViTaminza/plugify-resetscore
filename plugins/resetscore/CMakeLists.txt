# resetscore — Plugify C++ plugin: port of the Pisex Metamod:Source plugin "ResetScore"
# ("mm_rs"/"sm_rs"/"rs"/"!rs"/chat word "rs" resets the caller's score, MVPs and match stats).
#
# The plugin calls s2sdk directly (PLUGIN_STANDARD §7: no pisexcompat, no guard library); its
# safety checks - the map gate, the slot/controller validation, the map-gated schema offsets and
# the command source dispatch - live in src/plugin.cpp, and the engine-free parts of them sit in
# the headers next to it so resetscore_tests can test them.
#
#   plugify-resetscore  the plugin shared library (entry "bin/resetscore")

set(RESETSCORE_VERSION "1.2.0")

add_library(plugify-resetscore SHARED
        src/plugin.cpp
        # The __s2sdk_* binding globals the plugify module fills from the manifest's dependencies.
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(plugify-resetscore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-resetscore SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-resetscore PRIVATE
        plugify_sdk
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

# --- tests --------------------------------------------------------------------------------------
# resetscore_tests: the shipped phrases file through the plugin's KV1 reader (language key, ru/en/
# ua/pl columns, fallback, malformed input), the colour table byte-for-byte, the bare chat word
# matcher, the schema-write plan and the map-gated schema offset cache. No engine, no Plugify host:
# only the plugin's own headers are compiled in.
add_executable(resetscore_tests tests/resetscore_tests.cpp)
target_include_directories(resetscore_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(resetscore_tests PRIVATE -static-libstdc++ -static-libgcc)
target_compile_options(resetscore_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(resetscore_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        RS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
set_target_properties(resetscore_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
