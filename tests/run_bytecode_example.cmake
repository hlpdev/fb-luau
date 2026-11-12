# Runs examples/bytecode's two modes, as a game's build and the game would be run: the build into
# a folder of the build tree's, then the game from it. Its output is theirs, and it fails when
# either does.
# cmake -DFB=<fb> -DPROGRAM=<folder> -DOUT=<folder> -P run_bytecode_example.cmake
foreach(mode IN ITEMS build game)
  execute_process(COMMAND "${FB}" --color never run "${PROGRAM}" -- ${mode} "${OUT}"
                  RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "the ${mode} failed: ${result}")
  endif()
endforeach()
