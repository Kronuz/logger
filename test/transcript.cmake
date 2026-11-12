# Runs the transcript and fails unless its output is identical, byte for byte, to the golden one the
# implementation before the producer path changed wrote.
#   cmake -DEXE=<exe> -DGOLDEN=<file> -DDIR=<scratch dir> -P transcript.cmake
file(MAKE_DIRECTORY "${DIR}")
execute_process(COMMAND "${EXE}" OUTPUT_FILE "${DIR}/transcript.txt" ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the transcript failed (${rc}): ${err}")
endif()
file(SIZE "${DIR}/transcript.txt" size)
if(size LESS 5000)
  message(FATAL_ERROR "the transcript is only ${size} bytes: the script did not run")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${DIR}/transcript.txt" "${GOLDEN}" RESULT_VARIABLE same)
if(NOT same EQUAL 0)
  execute_process(COMMAND diff -u "${GOLDEN}" "${DIR}/transcript.txt" OUTPUT_VARIABLE d)
  string(SUBSTRING "${d}" 0 6000 d)
  message(FATAL_ERROR "the logger writes different bytes than the golden transcript:\n${d}")
endif()
message(STATUS "transcript identical (${size} bytes)")
