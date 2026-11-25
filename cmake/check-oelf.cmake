# Verify generated PS4 OELF has valid PT_LOAD structure (exactly 1 RW PT_LOAD segment)
# Adapted from ps4-kit check-oelf.cmake (MIT License)
find_program(READELF NAMES llvm-readelf-18 llvm-readelf readelf)
if (NOT READELF)
    message(WARNING "check-oelf: no readelf found, skipping PT_LOAD verification")
    return()
endif ()

execute_process(
    COMMAND ${READELF} -l -W ${OELF}
    OUTPUT_VARIABLE out
    RESULT_VARIABLE rc
)
if (NOT rc EQUAL 0)
    message(FATAL_ERROR "check-oelf: ${READELF} failed on ${OELF}")
endif ()

string(REGEX MATCHALL "\n *LOAD +[^\n]* RW  [^\n]*" rw_loads "${out}")
list(LENGTH rw_loads count)
if (NOT count EQUAL 1)
    message(FATAL_ERROR "check-oelf: ${OELF} has ${count} read-write PT_LOAD segments (expected 1).\n${out}")
endif ()
message(STATUS "check-oelf: ${OELF} verified OK (1 RW PT_LOAD segment)")
