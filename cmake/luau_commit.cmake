# Checks the Luau checkout at `source` is commit `expected`, the one its tag named when the package
# was pinned to it, and stops the configure when it isn't, as a tag can be moved. Run as a script,
# `cmake -DSOURCE=<dir> -DEXPECTED=<hash> -P luau_commit.cmake`, it checks those, which its test
# does.
function(fbl_check_luau_commit source expected)
  find_package(Git QUIET REQUIRED)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${source}" rev-parse HEAD
                  OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE failed ERROR_QUIET)
  if(failed)
    message(FATAL_ERROR "can't tell which commit Luau's checkout at ${source} is")
  endif()
  if(NOT commit STREQUAL expected)
    message(FATAL_ERROR "Luau's tag has moved: it's commit ${commit}, not ${expected}, the one "
                        "the package was pinned to; take a new one by hand, as cmake/luau.cmake says")
  endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  fbl_check_luau_commit("${SOURCE}" "${EXPECTED}")
  message(STATUS "Luau's checkout is commit ${EXPECTED}")
endif()
