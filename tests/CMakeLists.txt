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

# The tests' threads, std::thread's, which glibc before 2.34 has in libpthread.
find_package(Threads REQUIRED)

# The shim's tests, one executable for each area, over fbl_internal, so they reach what the
# library doesn't export, and with the library itself for what's only true of it: that it loads,
# and what it exports.
function(fbl_add_tests area)
  add_executable(fbl_${area}_tests ${ARGN})
  fbl_set_warnings(fbl_${area}_tests)
  target_link_libraries(fbl_${area}_tests PRIVATE fbl_internal Catch2::Catch2WithMain
                        Threads::Threads)
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
fbl_add_tests(bytecode bytecode/bytecode_tests.cpp)
fbl_add_tests(check check/check_tests.cpp)
# The cases the checker's diagnostics are compared with luau-analyze's on (tools/analyze_golden.py).
target_compile_definitions(fbl_check_tests PRIVATE
                           FBL_CHECK_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/check/corpus")
fbl_add_tests(native native/native_tests.cpp)

find_program(FBL_SH sh DOC "A POSIX shell, for the tools' scripts")
find_program(FBL_PYTHON NAMES python3 python DOC "Python 3, for the tools' checks")

# The export check passes on the library, and refuses one that exports more than its C API, so
# it's known to look.
if(FBL_SH)
  # The check that Luau's tag is still its commit stops a configure when it isn't.
  add_test(NAME tools.luau_commit
           COMMAND "${CMAKE_COMMAND}" -DSOURCE=${luau_SOURCE_DIR} -DEXPECTED=${FBL_LUAU_COMMIT}
                   -P "${PROJECT_SOURCE_DIR}/cmake/luau_commit.cmake")
  add_test(NAME tools.luau_commit_moved
           COMMAND "${CMAKE_COMMAND}" -DSOURCE=${luau_SOURCE_DIR}
                   -DEXPECTED=0000000000000000000000000000000000000000
                   -P "${PROJECT_SOURCE_DIR}/cmake/luau_commit.cmake")
  set_tests_properties(tools.luau_commit_moved PROPERTIES
                       PASS_REGULAR_EXPRESSION "Luau's tag has moved")
  add_test(NAME tools.check_exports
           COMMAND "${FBL_SH}" "${PROJECT_SOURCE_DIR}/tools/check-exports.sh" fbl_
                   "$<TARGET_FILE:luau_native>")
  set_tests_properties(tools.check_exports PROPERTIES
                       PASS_REGULAR_EXPRESSION "loads only the system's libraries")
  # Built as the library is, but with no prefix to keep to, so its stray name is exported: with
  # fbl_ the macOS export list and the Linux version script would hide it, as they do the
  # library's.
  fbl_add_shared(fbl_overexported "" "" tools/overexported.cpp)
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
      "table-after-close=the Luau State is closed"
      "buffer-after-close=the Luau State is closed"
      "script-after-close=the Luau State is closed"
      "other-state-handle=a LuauTable of another Luau State"
      "buffer-write-past-end=3 bytes at 2 aren't within a buffer of 4"
      "dropped-state=the Luau State is closed"
      "unregistered=Door isn't registered"
      "buffer-bounds=4 bytes at 1 aren't within a buffer of 4"
      "buffer-read-huge=1099511627776 bytes at 0 aren't within a buffer of 4"
      "eight-arguments=Many.Sum is marked @LuauExpose, but isn't"
      "get-argument=Asking.Size is marked @LuauGet, but isn't"
      "set-none=Unset.Reset is marked @LuauSet, but isn't"
      "two-names=Twice's Damage and hurt are both 'hurt' in Luau"
      "closed-builder=the Luau State is closed"
      "builder-other-state=a LuauFunction of another Luau State"
      "negative-limit=a memory limit of -1 bytes is below 0"
      "short-key=an Ed25519 private key is 32 bytes, not 5"
      "short-public-key=an Ed25519 public key is 32 bytes, not 5"
      "nul-name=a chunk's name can't hold a NUL"
      "negative-budget=a time budget of -1 microseconds is below 0"
      "negative-native-memory=a native code cap of -1 bytes is below 0"
      "optimization=a compile option of 3 isn't from 0 to 2"
      "state-started-twice=the Luau State's driver runs already"
      "checker-time-limit=a checker's time limit of 0"
      "checker-heap-limit=0 bytes isn't a type function's heap limit"
      "action-gives=Acting.Count is marked @LuauAction, but isn't"
      "throwing-shape=Throwing.Take is marked @LuauExpose, but isn't"
      "shadowed=Shadowed.Harm is marked @LuauExpose, but isn't")
  # The examples, which run as programs do.
  add_test(NAME examples.bytecode
           COMMAND "${CMAKE_COMMAND}" -DFB=${FBL_FB} -DPROGRAM=${PROJECT_SOURCE_DIR}/examples/bytecode
                   -DOUT=${PROJECT_BINARY_DIR}/bytecode-example
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/run_bytecode_example.cmake")
  set_tests_properties(examples.bytecode PROPERTIES
                       PASS_REGULAR_EXPRESSION "built 2 containers.*hello, player.*door opened.*a changed container is refused.*signed by another key is refused")

  # The guide's programs, each run as its own.
  if(FBL_PYTHON)
    add_test(NAME docs.guide_programs
             COMMAND "${FBL_PYTHON}" "${PROJECT_SOURCE_DIR}/tools/guide_programs.py" "${FBL_FB}"
                     "${PROJECT_BINARY_DIR}/guide")
  endif()
  # Luau's conformance scripts through the package, each that can run here.
  add_test(NAME luau.conformance
           COMMAND "${FBL_FB}" --color never run --release "${CMAKE_CURRENT_SOURCE_DIR}/conformance")
  set_tests_properties(luau.conformance PROPERTIES PASS_REGULAR_EXPRESSION " 0 failed")
  # A dry run of fb publish, which builds and checks the package's archives as a registry would,
  # against a registry in a folder of the build's, with this target's library.
  file(WRITE "${PROJECT_BINARY_DIR}/registry/config.json"
       "{\"api\":\"http://127.0.0.1:1/api/v1\",\"dl\":\"http://127.0.0.1:1/dl/{name}/{version}/{file}\",\"v\":1}
")
  add_test(NAME tools.publish_dry_run
           COMMAND "${FBL_FB}" --color never publish --dry-run --offline
                   --registry "${PROJECT_BINARY_DIR}/registry" "${PROJECT_SOURCE_DIR}/luau")
  set_tests_properties(tools.publish_dry_run PROPERTIES
                       PASS_REGULAR_EXPRESSION "Finished the dry run of luau v")
  # The bytecode container's fuzzer, in FutureBlitz, for 5 seconds; stability.yml's is 10 minutes.
  add_test(NAME fuzz.containers
           COMMAND "${FBL_FB}" --color never run --release "${PROJECT_SOURCE_DIR}/fuzz/containers" -- 5)
  set_tests_properties(fuzz.containers PROPERTIES PASS_REGULAR_EXPRESSION "inputs, no finding")
  # The soak, short: stability.yml runs it for 30 minutes.
  add_test(NAME luau.soak
           COMMAND "${FBL_FB}" --color never run --release "${CMAKE_CURRENT_SOURCE_DIR}/soak" -- 15 9)
  set_tests_properties(luau.soak PROPERTIES PASS_REGULAR_EXPRESSION "soak: [0-9]+ iterations"
                       FAIL_REGULAR_EXPRESSION "a leak|outlived its close")
  add_test(NAME examples.repl
           COMMAND "${CMAKE_COMMAND}" -DFB=${FBL_FB} -DPROGRAM=${PROJECT_SOURCE_DIR}/examples/repl
                   -DINPUT=${PROJECT_SOURCE_DIR}/examples/repl/session.txt
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/run_with_input.cmake")
  set_tests_properties(examples.repl PROPERTIES
                       PASS_REGULAR_EXPRESSION "> 3
.*> 20
> hello from luau
> stdin:1: oops
> a	true	nil
.*> 3
")
  add_test(NAME examples.modded_game
           COMMAND "${FBL_FB}" --color never run "${PROJECT_SOURCE_DIR}/examples/modded_game")
  set_tests_properties(examples.modded_game PROPERTIES
                       PASS_REGULAR_EXPRESSION "broken isn't loaded: 2 errors.*doors is loaded.*greedy is loaded.*hoarder isn't loaded: MEMORY.*looping isn't loaded: TIMEOUT.*spawner is loaded.*thrower isn't loaded: RUNTIME.*the thrower gives up.*twofold isn't loaded: RUNTIME.*2 files.*spawner spawned rat 1.*the front door is open.*FRONT IS OPEN!.*greedy is stopped at tick 5: TIMEOUT.*spawner spawned rat 3.*its waits ended: True")
  # No tick of the thrower's, whose hook went with it.
  set_tests_properties(examples.modded_game PROPERTIES FAIL_REGULAR_EXPRESSION "thrower ticks")

  # @LuauExpose on a Method that gives nothing, @LuauAction's: since fb 1.3.1 it compiles, and
  # Register names it in a panic.
  add_test(NAME luau.compile.expose-nothing
           COMMAND "${FBL_FB}" --color never run "${CMAKE_CURRENT_SOURCE_DIR}/compile")
  set_tests_properties(luau.compile.expose-nothing PROPERTIES
                       PASS_REGULAR_EXPRESSION "Silent.Nothing is marked @LuauExpose, but isn't")
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
