# Runs examples/bytecode's two modes, as a game's build and the game would be run: the build, given
# its private key from outside what it writes, into a folder of the build tree's, then the game,
# which has the public key compiled in, from it. Its output is theirs, and it fails when either
# does.
# cmake -DFB=<fb> -DPROGRAM=<folder> -DOUT=<folder> -P run_bytecode_example.cmake
execute_process(COMMAND "${FB}" --color never run "${PROGRAM}" -- build
                        "${PROGRAM}/keys/build.key" "${OUT}"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "the build failed: ${result}")
endif()
execute_process(COMMAND "${FB}" --color never run "${PROGRAM}" -- game "${OUT}"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "the game failed: ${result}")
endif()
