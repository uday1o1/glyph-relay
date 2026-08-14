if(NOT DEFINED GLYPHRELAY_PROGRAM OR NOT DEFINED GLYPHRELAY_OUTPUT)
  message(FATAL_ERROR "record CLI assertion requires program and output")
endif()

execute_process(
  COMMAND "${GLYPHRELAY_PROGRAM}" record --output "${GLYPHRELAY_OUTPUT}"
          --window-id forbidden
  RESULT_VARIABLE result
  OUTPUT_VARIABLE standard_output
  ERROR_VARIABLE standard_error)

if(NOT result EQUAL 2)
  message(FATAL_ERROR
    "caller-selected source must exit 2, got ${result}: ${standard_output}${standard_error}")
endif()

if(EXISTS "${GLYPHRELAY_OUTPUT}" OR EXISTS "${GLYPHRELAY_OUTPUT}.journal")
  message(FATAL_ERROR "rejected caller-selected source created a recording artifact")
endif()
