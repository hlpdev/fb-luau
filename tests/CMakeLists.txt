FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.16.1
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# The shim's tests, one executable for each area, over fbl_internal, so they reach what the
# library doesn't export, and with the library itself for what's only true of it: that it loads,
# and what it exports.
function(fbl_add_tests area)
  add_executable(fbl_${area}_tests ${ARGN})
  fbl_set_warnings(fbl_${area}_tests)
  target_link_libraries(fbl_${area}_tests PRIVATE fbl_internal Catch2::Catch2WithMain)
  target_include_directories(fbl_${area}_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(fbl_${area}_tests PRIVATE
                             FBL_LIBRARY="$<TARGET_FILE:luau_native>"
                             FBL_VERSION="${FBL_VERSION}"
                             FBL_LUAU_VERSION="${FBL_LUAU_VERSION}")
  add_dependencies(fbl_${area}_tests luau_native)
  if(MINGW)
    # Run from anywhere, with no DLLs of MinGW's on the PATH.
    target_link_options(fbl_${area}_tests PRIVATE -static)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(fbl_${area}_tests PRIVATE ${CMAKE_DL_LIBS})
  endif()
  catch_discover_tests(fbl_${area}_tests TEST_PREFIX "${area}." ADD_TAGS_AS_LABELS)
endfunction()

fbl_add_tests(core core/core_tests.cpp)
fbl_add_tests(state state/state_tests.cpp)
fbl_add_tests(frame frame/frame_tests.cpp)
fbl_add_tests(call call/call_tests.cpp)
fbl_add_tests(table table/table_tests.cpp)
fbl_add_tests(callback callback/callback_tests.cpp)
fbl_add_tests(udata udata/udata_tests.cpp)
fbl_add_tests(buffer buffer/buffer_tests.cpp)
fbl_add_tests(sandbox sandbox/sandbox_tests.cpp)
fbl_add_tests(thread thread/thread_tests.cpp)
fbl_add_tests(require require/require_tests.cpp)

find_program(FBL_SH sh DOC "A POSIX shell, for the tools' scripts")
find_program(FBL_PYTHON NAMES python3 python DOC "Python 3, for the tools' checks")

# The export check passes on the library, and refuses one that exports more than its C API, so
# it's known to look.
if(FBL_SH)
  add_test(NAME tools.check_exports
           COMMAND "${FBL_SH}" "${PROJECT_SOURCE_DIR}/tools/check-exports.sh" fbl_
                   "$<TARGET_FILE:luau_native>")
  set_tests_properties(tools.check_exports PROPERTIES
                       PASS_REGULAR_EXPRESSION "loads only the system's libraries")
  fbl_add_shared(fbl_overexported fbl_ "" tools/overexported.cpp)
  target_include_directories(fbl_overexported PRIVATE "${PROJECT_SOURCE_DIR}/native")
  target_compile_definitions(fbl_overexported PRIVATE FBL_BUILDING)
  add_test(NAME tools.check_exports_refuses
           COMMAND "${FBL_SH}" "${PROJECT_SOURCE_DIR}/tools/check-exports.sh" fbl_
                   "$<TARGET_FILE:fbl_overexported>")
  set_tests_properties(tools.check_exports_refuses PROPERTIES
                       PASS_REGULAR_EXPRESSION "exports more than its fbl_ functions")
endif()

# The notices name every library the library links, as the build links it, and that check fails
# as it should. The link graph is CMake's file API's codemodel, which this asks for, and CMake
# answers as it generates.
if(FBL_PYTHON)
  cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)
  add_test(NAME tools.check_notices
           COMMAND "${FBL_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/check_notices.py"
                   "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}")
  set_tests_properties(tools.check_notices PROPERTIES
                       PASS_REGULAR_EXPRESSION "name every library it links")
  add_test(NAME tools.check_notices_test
           COMMAND "${FBL_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/check_notices_test.py")
  set_tests_properties(tools.check_notices_test PROPERTIES PASS_REGULAR_EXPRESSION "cases hold")
endif()

# The package's own tests, its doc comments' examples among them, run by `fb test` once the
# library is in luau/native/<target>/.
option(FBL_FB_TESTS "Test the package with fb" ON)
if(FBL_FB_TESTS)
  find_program(FBL_FB fb REQUIRED DOC "The fb that tests the package")
  add_test(NAME luau.package COMMAND "${FBL_FB}" test "${PROJECT_SOURCE_DIR}/luau")
  set_tests_properties(luau.package PROPERTIES TIMEOUT 600)

  # The package's panics, each a case of tests/panics run on its own, which has to panic saying
  # what it does.
  set(fbl_panics
      "closed-state=the Luau State is closed"
      "closed-function=the Luau State is closed"
      "dropped-state=the Luau State is closed"
      "unregistered=Door isn't registered"
      "buffer-bounds=4 bytes at 1 aren't within a buffer of 4"
      "negative-limit=a memory limit of -1 bytes is below 0"
      "negative-budget=a time budget of -1 microseconds is below 0"
      "optimization=a compile option of 3 isn't from 0 to 2"
      "started-twice=the Luau thread runs on the scheduler already"
      "resumed-by-hand=the Luau thread runs on the scheduler")
  foreach(case IN LISTS fbl_panics)
    string(FIND "${case}" "=" split)
    string(SUBSTRING "${case}" 0 ${split} name)
    math(EXPR split "${split} + 1")
    string(SUBSTRING "${case}" ${split} -1 message)
    add_test(NAME luau.panics.${name}
             COMMAND "${FBL_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/panics" -- ${name})
    set_tests_properties(luau.panics.${name} PROPERTIES PASS_REGULAR_EXPRESSION "panic: ${message}")
  endforeach()
endif()
