if(NOT DEFINED GLYPHRELAY_PROGRAM OR NOT DEFINED GLYPHRELAY_OUTPUT)
  message(FATAL_ERROR "CUDA saliency fixture requires program and output")
endif()

file(REMOVE "${GLYPHRELAY_OUTPUT}")
execute_process(
  COMMAND "${GLYPHRELAY_PROGRAM}" --correctness --output "${GLYPHRELAY_OUTPUT}"
  RESULT_VARIABLE first_result
  OUTPUT_VARIABLE first_output
  ERROR_VARIABLE first_error)
if(NOT first_result EQUAL 0 OR NOT first_output MATCHES "\\\"status\\\":\\\"PASSED\\\"")
  message(FATAL_ERROR "CUDA saliency correctness failed: ${first_result}: ${first_error}")
endif()

execute_process(
  COMMAND "${GLYPHRELAY_PROGRAM}" --correctness --output "${GLYPHRELAY_OUTPUT}"
  RESULT_VARIABLE second_result
  OUTPUT_VARIABLE second_output
  ERROR_VARIABLE second_error)
if(NOT second_result EQUAL 2 OR NOT second_error MATCHES "File exists")
  message(FATAL_ERROR "CUDA saliency qualification did not fail closed on overwrite")
endif()
