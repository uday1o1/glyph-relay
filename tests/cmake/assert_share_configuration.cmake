execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env --unset=GLYPHRELAY_SIGNALING_ORIGIN
          "${GLYPHRELAY_PROGRAM}" share --json
  RESULT_VARIABLE unconfigured_result
  OUTPUT_VARIABLE unconfigured_output
  ERROR_VARIABLE unconfigured_error)
if(NOT unconfigured_result EQUAL 2)
  message(FATAL_ERROR "unconfigured share returned ${unconfigured_result}")
endif()
if(NOT unconfigured_output MATCHES "signaling_origin_unconfigured")
  message(FATAL_ERROR "unconfigured share did not report its missing prerequisite")
endif()
if(unconfigured_output MATCHES "joinUrl\":\"[^\"]+")
  message(FATAL_ERROR "unconfigured share emitted a remote link")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env GLYPHRELAY_SIGNALING_ORIGIN=https://share.invalid
          "${GLYPHRELAY_PROGRAM}" share --origin https://attacker.invalid
  RESULT_VARIABLE override_result
  OUTPUT_VARIABLE override_output
  ERROR_VARIABLE override_error)
if(NOT override_result EQUAL 2)
  message(FATAL_ERROR "share accepted a public origin override")
endif()
if(NOT override_error MATCHES "accepts only --bitrate, --record, and --json")
  message(FATAL_ERROR "share origin override did not fail at CLI parsing")
endif()
