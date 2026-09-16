cmake_minimum_required(VERSION 3.20)
project(Dishonored2HeadTracking VERSION 0.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# MinHook (vendored). Built as its own target so cameraunlock_hooks can link
# against it; the objects end up statically linked into the .asi.
add_library(minhook STATIC
    extern/minhook/src/buffer.c
    extern/minhook/src/hook.c
    extern/minhook/src/trampoline.c
    extern/minhook/src/hde/hde32.c
    extern/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/minhook/include
)

set(CAMERAUNLOCK_BUILD_HOOKS ON CACHE BOOL "" FORCE)
add_subdirectory(cameraunlock-core/cpp ${CMAKE_BINARY_DIR}/cameraunlock-core)

set(MOD_SOURCES
    src/pch.cpp
    src/dllmain.cpp
    src/core/mod.cpp
    src/core/config.cpp
    src/core/game_window.cpp
    src/core/hotkeys.cpp
    src/core/path_utils.cpp
    src/hooks/build_profiles.cpp
    src/hooks/steam_offsets.cpp
    src/hooks/camera_hook.cpp
    src/hooks/game_state.cpp
    src/hooks/reticle_hook.cpp
    src/hooks/marker_hook.cpp
    src/voidengine/game_singletons.cpp
    src/voidengine/world_trace.cpp
)

add_library(Dishonored2HeadTracking SHARED ${MOD_SOURCES})

target_precompile_headers(Dishonored2HeadTracking PRIVATE src/pch.h)

# C++20 for the designated initialisers the build-profile registry is written
# with. Every RVA in a profile is a bare uint32_t, so a positional initialiser
# list re-routes every value below an inserted field, silently, in a build that
# still fingerprint-matches and still installs its hooks.
target_compile_features(Dishonored2HeadTracking PRIVATE cxx_std_20)

# The mod is dense with mixed-width pointer arithmetic, which the compiler
# polices for free at /W4. /WX is what keeps it policed.
if(MSVC)
    target_compile_options(Dishonored2HeadTracking PRIVATE /W4 /WX /permissive-)
endif()

set_target_properties(Dishonored2HeadTracking PROPERTIES
    OUTPUT_NAME "Dishonored2HeadTracking"
    SUFFIX ".asi"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)

target_link_libraries(Dishonored2HeadTracking PRIVATE
    cameraunlock
    cameraunlock_hooks
)

target_include_directories(Dishonored2HeadTracking PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
    target_link_libraries(Dishonored2HeadTracking PRIVATE
        ws2_32
    )
endif()

option(D2HT_BUILD_TESTS "Build mod unit tests" OFF)
if(D2HT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
