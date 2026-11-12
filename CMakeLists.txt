cmake_minimum_required(VERSION 3.28)

# The package's version, which luau/project.toml says, and which the library says too. A
# prerelease, as 1.0.0-rc.1, is the whole of FBL_VERSION; CMake's project version takes its
# numbers alone.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/luau/project.toml" fbl_version_line
     REGEX "^version = \"[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?\"$" LIMIT_COUNT 1)
if(NOT fbl_version_line MATCHES "\"(([0-9]+\\.[0-9]+\\.[0-9]+)(-[0-9A-Za-z.-]+)?)\"")
  message(FATAL_ERROR "luau/project.toml has no version like \"0.1.0\" or \"1.0.0-rc.1\"")
endif()
set(FBL_VERSION "${CMAKE_MATCH_1}")
project(fb_luau VERSION ${CMAKE_MATCH_2} LANGUAGES C CXX)
# A new version there configures again, so the library says it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/luau/project.toml")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++ modules here; scanning adds GCC-only flags that break clang-tidy and clangd.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(FBL_BUILD_TESTS "Build the tests" ON)

# A sanitizer's build, as .github/workflows/stability.yml makes: -fsanitize's list, such as
# address,undefined or thread, for every target compiled here, Luau's too, as ThreadSanitizer
# needs all of what runs built with it. Empty, as every other build has it, it's none.
set(FBL_SANITIZE "" CACHE STRING "-fsanitize's list for every target, or empty for none")
if(FBL_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "FBL_SANITIZE is for GCC and Clang")
  endif()
  # A sanitizer's finding stops the test, rather than only being printed.
  add_compile_options(-fsanitize=${FBL_SANITIZE} -fno-sanitize-recover=all
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${FBL_SANITIZE})
endif()

# libFuzzer's build, as stability.yml makes with Clang: every target compiled for its coverage, and
# the fuzzers linked with its driver. Without it, each fuzzer is linked with fuzz/replay.cpp, which
# runs its corpus and random inputs as a test.
option(FBL_FUZZ "Build the fuzzers with libFuzzer (Clang)" OFF)
if(FBL_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "FBL_FUZZ is for Clang, whose libFuzzer it links")
  endif()
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# MSVC otherwise reads source files in the ANSI code page.
if(MSVC)
  add_compile_options(/utf-8)
endif()

include(FetchContent)
include(cmake/warnings.cmake)
include(cmake/native.cmake)
include(cmake/luau.cmake)

add_subdirectory(native)

if(FBL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  add_subdirectory(fuzz)
endif()
