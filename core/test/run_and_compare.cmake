# cmake -DCOMMAND=prog|arg|... -DOUT=file -DEXPECTED=file -P run_and_compare.cmake
# Runs COMMAND (arguments separated by |), then requires OUT to equal
# EXPECTED byte for byte. A shell-free `prog args && cmp OUT EXPECTED`.
string(REPLACE "|" ";" command "${COMMAND}")
execute_process(COMMAND ${command} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "${command}: exit ${result}")
endif()
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${OUT}" "${EXPECTED}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "${OUT} differs from ${EXPECTED}")
endif()
