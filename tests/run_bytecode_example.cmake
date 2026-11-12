# Runs examples/bytecode as a game's would be: its key pair made once, where there's none, then the
# build, given its private key from outside what it writes, into a folder of the build tree's,
# then the game, which has the public key compiled in, from it. Its output is theirs, and it fails
# when any part does.
# cmake -DFB=<fb> -DPROGRAM=<folder> -DOUT=<folder> -P run_bytecode_example.cmake
execute_process(COMMAND "${FB}" --color never run "${PROGRAM}/keygen" --
                        "${PROGRAM}/keys/build.key" "${PROGRAM}/game.pub"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "the key pair wasn't made: ${result}")
endif()
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
