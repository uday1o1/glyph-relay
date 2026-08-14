if(NOT DEFINED GLYPHRELAY_PROGRAM OR NOT DEFINED GLYPHRELAY_OUTPUT)
  message(FATAL_ERROR "saliency preview fixture requires program and output")
endif()

file(REMOVE "${GLYPHRELAY_OUTPUT}")
execute_process(
  COMMAND "${GLYPHRELAY_PROGRAM}" --output "${GLYPHRELAY_OUTPUT}"
          --pin 80,80,96,48 --exclude 112,96,24,16
  RESULT_VARIABLE first_result
  OUTPUT_VARIABLE first_output
  ERROR_VARIABLE first_error)
if(NOT first_result EQUAL 0 OR NOT first_output MATCHES "\\\"status\\\":\\\"PASSED\\\"" OR
   NOT first_output MATCHES "\\\"correctionRevision\\\":2" OR
   NOT first_output MATCHES "\\\"conflictTileCount\\\":[1-9]")
  message(FATAL_ERROR "saliency preview failed: ${first_result}: ${first_error}")
endif()
file(READ "${GLYPHRELAY_OUTPUT}" magic LIMIT 2 HEX)
if(NOT magic STREQUAL "5036")
  message(FATAL_ERROR "saliency preview is not a binary PPM")
endif()

execute_process(
  COMMAND "${GLYPHRELAY_PROGRAM}" --output "${GLYPHRELAY_OUTPUT}"
          --pin 80,80,96,48 --exclude 112,96,24,16
  RESULT_VARIABLE second_result
  OUTPUT_VARIABLE second_output
  ERROR_VARIABLE second_error)
if(NOT second_result EQUAL 2 OR NOT second_error MATCHES "File exists")
  message(FATAL_ERROR "saliency preview did not fail closed on overwrite")
endif()
