cmake_minimum_required(VERSION 3.20)

# Mod-level unit tests for src/core (config validation and INI round-trip) and
# for the UDP port recovery the mod depends on. Links the cameraunlock static
# lib for IniReader/IniWriter and UdpReceiver, plus ws2_32 for the test's own
# contending socket; no MinHook is needed by the units under test.
add_executable(d2ht_tests
    d2ht_tests.cpp
    ${CMAKE_SOURCE_DIR}/src/core/config.cpp
    ${CMAKE_SOURCE_DIR}/src/voidengine/world_trace.cpp
)

target_include_directories(d2ht_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(d2ht_tests PRIVATE cameraunlock)

target_compile_features(d2ht_tests PRIVATE cxx_std_20)

# So the shipped-ini test can find the file it is checking.
target_compile_definitions(d2ht_tests PRIVATE D2HT_REPO_ROOT="${CMAKE_SOURCE_DIR}")

if(MSVC)
    target_compile_options(d2ht_tests PRIVATE /W4 /WX /permissive-)
endif()

if(WIN32)
    target_link_libraries(d2ht_tests PRIVATE ws2_32)
endif()

add_test(NAME d2ht_tests COMMAND d2ht_tests)
