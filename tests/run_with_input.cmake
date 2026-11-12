# Runs a program with a file as its standard input, for an example that reads it, as ctest can't:
# cmake -DFB=<fb> -DPROGRAM=<folder> -DINPUT=<file> -P run_with_input.cmake. Its output is the
# program's, and it fails when the program does.
execute_process(COMMAND "${FB}" --color never run "${PROGRAM}"
                INPUT_FILE "${INPUT}"
                RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "${PROGRAM} failed: ${result}")
endif()
