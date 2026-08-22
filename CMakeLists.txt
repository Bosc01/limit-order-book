cmake_minimum_required(VERSION 3.20)
project(lob LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# This machine's CommandLineTools install is missing the toolchain copy of
# the libc++ headers, and clang does not fall back to the SDK's copy on its
# own. Add the SDK copy only when the toolchain copy is absent.
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"
   AND NOT EXISTS "/Library/Developer/CommandLineTools/usr/include/c++/v1/cstdint")
  execute_process(COMMAND xcrun --show-sdk-path
                  OUTPUT_VARIABLE LOB_SDKROOT
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  include_directories(SYSTEM "${LOB_SDKROOT}/usr/include/c++/v1")
endif()

# Hot-path flag rationale:
#   -O3             aggressive inlining: the hot path is many tiny functions
#                   that must collapse into one straight-line matching loop.
#   -fno-exceptions no unwind tables, and the compiler stops treating every
#                   call as "may throw", which frees it to keep state in
#                   registers across calls. Hot-path errors are return codes.
#   -fno-rtti       no typeinfo; documents that nothing on the hot path is
#                   virtual (engines use templates, not interfaces).
set(LOB_HOT_FLAGS -O3 -fno-exceptions -fno-rtti -g -Wall -Wextra -Wpedantic)

# Optional sanitizer build: cmake -DLOB_SANITIZE="address,undefined"
# Applied to TEST targets (sanitizers change codegen too much for benchmark
# numbers to mean anything).
set(LOB_SANITIZE "" CACHE STRING "sanitizers for test targets, e.g. address,undefined")

# ---- benchmark ------------------------------------------------------------
add_executable(bench bench/main.cpp bench/alloc_count.cpp)
target_include_directories(bench PRIVATE include bench)
target_compile_options(bench PRIVATE ${LOB_HOT_FLAGS})

# ---- tests ----------------------------------------------------------------
# GoogleTest: use a local checkout in third_party/ when present (offline
# builds), otherwise fetch the pinned tag. Tests build WITH exceptions
# (GoogleTest wants them) and at -O1: the engine headers are exception-free
# either way, and sanitizer/assert quality matters more than speed here.
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
  add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest
    GIT_TAG        v1.17.0)
  FetchContent_MakeAvailable(googletest)
endif()

# ---- networking (Phase 4: real, not fast) ---------------------------------
add_executable(gateway net/gateway.cpp)
add_executable(order_client net/order_client.cpp)
add_executable(feed_client net/feed_client.cpp)
foreach(t gateway order_client feed_client)
  target_include_directories(${t} PRIVATE include .)
  target_compile_options(${t} PRIVATE ${LOB_HOT_FLAGS})
endforeach()

add_executable(lob_tests
  tests/engine_semantics_test.cpp
  tests/v1_features_test.cpp
  tests/flat_engine_test.cpp
  tests/pool_test.cpp
  tests/id_map_test.cpp
  tests/net_protocol_test.cpp
  tests/differential_test.cpp
)
target_include_directories(lob_tests PRIVATE include bench .)
target_compile_options(lob_tests PRIVATE -O1 -g -Wall -Wextra)
target_link_libraries(lob_tests PRIVATE GTest::gtest_main)

if(LOB_SANITIZE)
  string(REPLACE "," ";" _san_list "${LOB_SANITIZE}")
  foreach(s ${_san_list})
    target_compile_options(lob_tests PRIVATE -fsanitize=${s} -fno-omit-frame-pointer)
    target_link_options(lob_tests PRIVATE -fsanitize=${s})
  endforeach()
endif()

# Coverage build: cmake -DLOB_COVERAGE=ON with an LLVM toolchain, then
#   LLVM_PROFILE_FILE=cov.profraw ./lob_tests
#   llvm-profdata merge -sparse cov.profraw -o cov.profdata
#   llvm-cov report ./lob_tests -instr-profile=cov.profdata
option(LOB_COVERAGE "instrument lob_tests for llvm source-based coverage" OFF)
if(LOB_COVERAGE)
  target_compile_options(lob_tests PRIVATE -fprofile-instr-generate -fcoverage-mapping)
  target_link_options(lob_tests PRIVATE -fprofile-instr-generate)
endif()

enable_testing()
include(GoogleTest)
if(NOT LOB_SANITIZE)
  gtest_discover_tests(lob_tests DISCOVERY_TIMEOUT 60)
else()
  # ASan startup makes post-build discovery time out; sanitizer builds are
  # run by invoking ./lob_tests directly, so ctest discovery adds nothing.
  add_test(NAME lob_tests COMMAND lob_tests)
endif()
