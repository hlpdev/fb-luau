# The fuzzers, over the shim's C API, through fbl_internal. With FBL_FUZZ, each is a libFuzzer
# program, which stability.yml runs for 10 minutes from its corpus; without it, each is linked with
# replay.cpp, and ctest runs it over its corpus and a few thousand random inputs, so what a fuzzer
# found stays found on every target.
set(fbl_fuzzers load bytecode api check)

foreach(fuzzer IN LISTS fbl_fuzzers)
  if(FBL_FUZZ)
    add_executable(fbl_fuzz_${fuzzer} fuzz_${fuzzer}.cpp)
    target_link_options(fbl_fuzz_${fuzzer} PRIVATE -fsanitize=fuzzer)
  else()
    add_executable(fbl_fuzz_${fuzzer} fuzz_${fuzzer}.cpp replay.cpp)
  endif()
  fbl_set_warnings(fbl_fuzz_${fuzzer})
  target_link_libraries(fbl_fuzz_${fuzzer} PRIVATE fbl_internal)
  target_include_directories(fbl_fuzz_${fuzzer} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  if(MINGW)
    target_link_options(fbl_fuzz_${fuzzer} PRIVATE -static)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(fbl_fuzz_${fuzzer} PRIVATE ${CMAKE_DL_LIBS})
  endif()
  if(NOT FBL_FUZZ)
    # The corpora start from Luau's conformance scripts, but the API's, which is calls.
    set(fbl_corpus "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${fuzzer}")
    if(NOT fuzzer STREQUAL "api")
      list(APPEND fbl_corpus "${PROJECT_SOURCE_DIR}/tests/conformance/scripts")
    endif()
    # The checker's inputs take longest, each up to its time limit.
    set(fbl_randoms 2000)
    if(fuzzer STREQUAL "check")
      set(fbl_randoms 300)
    endif()
    add_test(NAME fuzz.${fuzzer} COMMAND fbl_fuzz_${fuzzer} ${fbl_corpus} --random ${fbl_randoms})
    set_tests_properties(fuzz.${fuzzer} PROPERTIES PASS_REGULAR_EXPRESSION "inputs, no finding")
  endif()
endforeach()
