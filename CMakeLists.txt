cmake_minimum_required(VERSION 3.20)
project(panaudia-core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings — applied per-target below (not globally, to avoid polluting msquic/opus builds)
if(MSVC)
    set(PANAUDIA_WARNINGS /W4 /wd4324)
else()
    set(PANAUDIA_WARNINGS ${PANAUDIA_WARNINGS})
endif()

# --- Dependencies (via FetchContent) ---

include(FetchContent)

# Catch2 — test framework
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

# msquic — QUIC transport (needed from Phase 3)
# Pinned to same commit as UE plugin: v2.6.0
set(QUIC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(QUIC_BUILD_PERF OFF CACHE BOOL "" FORCE)
set(QUIC_ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    msquic
    GIT_REPOSITORY https://github.com/microsoft/msquic.git
    GIT_TAG ed14762f5d55d938aad1c99cbcb287713a0f1e2f
)
FetchContent_MakeAvailable(msquic)

# libopus — audio codec
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(opus)

# --- Library ---

add_library(panaudia-core STATIC
    src/panaudia_core.cpp
    src/session_manager.cpp
    src/moq_transport.cpp
    src/moq_protocol.cpp
    src/opus_codec.cpp
    src/ring_buffer.cpp
    src/jitter_buffer.cpp
    src/send_worker.cpp
    src/recv_worker.cpp
)

target_include_directories(panaudia-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(panaudia-core PRIVATE ${PANAUDIA_WARNINGS})

# msquic only exposes headers via INSTALL_INTERFACE, so add BUILD_INTERFACE manually
target_include_directories(panaudia-core
    PRIVATE
        ${msquic_SOURCE_DIR}/src/inc
)

target_link_libraries(panaudia-core
    PRIVATE
        Opus::opus
        msquic
)

# --- Tests ---

add_executable(panaudia-core-tests
    tests/test_ring_buffer.cpp
    tests/test_jitter_buffer.cpp
    tests/test_opus_codec.cpp
    tests/test_moq_protocol.cpp
    tests/test_moq_transport.cpp
    tests/test_session.cpp
)

target_compile_options(panaudia-core-tests PRIVATE ${PANAUDIA_WARNINGS})

target_link_libraries(panaudia-core-tests
    PRIVATE
        panaudia-core
        Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(panaudia-core-tests)

# --- Benchmarks ---

add_executable(panaudia-bench
    tests/bench_buffers.cpp
    tests/bench_codec.cpp
)

target_compile_options(panaudia-bench PRIVATE ${PANAUDIA_WARNINGS})

target_link_libraries(panaudia-bench
    PRIVATE
        panaudia-core
        Catch2::Catch2WithMain
)

# --- Test Harness ---

add_executable(panaudia-test-harness
    tests/test_harness.cpp
)

target_compile_options(panaudia-test-harness PRIVATE ${PANAUDIA_WARNINGS})

target_link_libraries(panaudia-test-harness
    PRIVATE
        panaudia-core
)

# --- Install ---

install(TARGETS panaudia-core
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/panaudia
    DESTINATION include
)