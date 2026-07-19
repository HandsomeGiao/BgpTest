if(NOT DEFINED CLI_EXECUTABLE OR NOT DEFINED SCRIPT OR NOT DEFINED WORK_DIR OR NOT DEFINED SCENARIO)
    message(FATAL_ERROR "CLI_EXECUTABLE, SCRIPT, WORK_DIR and SCENARIO are required")
endif()

set(command "${CLI_EXECUTABLE}")
if(DEFINED TOPOLOGY AND NOT TOPOLOGY STREQUAL "")
    list(APPEND command --topology "${TOPOLOGY}")
endif()
list(APPEND command --script "${SCRIPT}" --no-record)

execute_process(
    COMMAND ${command}
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE diagnostics
    ENCODING UTF-8
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "BgpTesterCli exited with ${result}\n${diagnostics}\n${output}")
endif()

string(REGEX MATCHALL "\"type\":\"command_started\"" started_records "${output}")
string(REGEX MATCHALL "\"type\":\"command_result\"" result_records "${output}")
list(LENGTH started_records started_count)
list(LENGTH result_records result_count)
if(started_count EQUAL 0 OR NOT started_count EQUAL result_count)
    message(FATAL_ERROR "CLI audit pairing is incomplete: started=${started_count}, results=${result_count}")
endif()

if(SCENARIO STREQUAL "runtime")
    foreach(required
            "\"path\":[\"C1\",\"RR1\",\"EDGE\",\"ISP\"]"
            "\"found\":true"
            "\"database_max_event_id\":"
            "\"committed_event_id\":"
            "\"runtime_originated_prefixes\":")
        string(FIND "${output}" "${required}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "Runtime CLI output is missing semantic evidence: ${required}")
        endif()
    endforeach()
elseif(SCENARIO STREQUAL "edit")
    foreach(required
            "\"seed\":12345"
            "\"changed_mrai_directions\":2"
            "\"new_id\":\"EDGE3\""
            "\"valid\":true")
        string(FIND "${output}" "${required}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "Edit CLI output is missing semantic evidence: ${required}")
        endif()
    endforeach()
else()
    message(FATAL_ERROR "Unknown CLI scenario: ${SCENARIO}")
endif()
