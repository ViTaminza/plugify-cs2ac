# cs2ac — Plugify C++ plugin: the CS2AC 1.0.14 server-side anti-cheat (originally a
# Metamod:Source plugin) ported to s2sdk + polyhook.
#
#   cs2ac_core            engine-independent pieces (detection math shared with the tests)
#   plugify-cs2ac         the plugin shared library (entry "bin/cs2ac")
#
# Everything that talks to the engine is s2sdk (data) or polyhook (code hooks); see PORT_PLAN.md
# for the module-by-module mapping and the list of deliberately unported parts.

set(CS2AC_VERSION "1.0.15")

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cs2ac.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cs2ac.pplugin"
)

# Engine-independent detection math: also the unit-test target (tests/cs2ac_tests.cpp), so it must
# not reference a single s2sdk/polyhook symbol.
add_library(cs2ac_core STATIC
        src/core.cpp
        src/config.cpp
        src/localization.cpp
)
target_include_directories(cs2ac_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(cs2ac_core SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_options(cs2ac_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(cs2ac_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

add_library(plugify-cs2ac SHARED
        src/plugin.cpp
        src/engine.cpp
        src/hooks.cpp
        src/announce.cpp
        src/webhook.cpp
        src/detection.cpp
        src/mod_aim.cpp
        src/mod_aimlock.cpp
        src/mod_antiaim.cpp
        src/mod_shots.cpp
        src/mod_misc.cpp
        src/mod_movement.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:polyhook_bindings>
)
target_include_directories(plugify-cs2ac PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/polyhook/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-cs2ac SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-cs2ac PRIVATE
        cs2ac_core
        -static-libstdc++ -static-libgcc
        ${CMAKE_DL_LIBS}
)
target_compile_options(plugify-cs2ac PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-cs2ac PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        CS2AC_VERSION="${CS2AC_VERSION}"
        PLUGIFY_MODULE="cs2ac"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-cs2ac PROPERTIES OUTPUT_NAME cs2ac)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-cs2ac PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

# Unit tests: pure detection math and the scoring rules of the original (the original's
# static_asserts are re-run as runtime assertions here). Run by ./test.sh.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/cs2ac_tests.cpp")
    add_executable(cs2ac_tests tests/cs2ac_tests.cpp)
    target_link_libraries(cs2ac_tests PRIVATE cs2ac_core -static-libstdc++ -static-libgcc)
    target_compile_options(cs2ac_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(cs2ac_tests PRIVATE
            CS2AC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            _GLIBCXX_USE_CXX11_ABI=1)
    set_target_properties(cs2ac_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
