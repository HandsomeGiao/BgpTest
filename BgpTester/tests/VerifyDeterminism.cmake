if(NOT DEFINED TEST_EXECUTABLE OR TEST_EXECUTABLE STREQUAL "")
    message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()

set(variants
    "16384,0,0,UTC,default"
    "1,40,1,Asia/Shanghai,0"
    "7,0,2,UTC,0"
    "3,25,0,Asia/Shanghai,default"
)
set(expected_digest "07160028aa369d5b86b5937e66e1abf7eea40585bc9f8c998495e12ca38282bc")
set(reference "")

foreach(repetition RANGE 1 2)
    foreach(variant IN LISTS variants)
        string(REPLACE "," ";" arguments "${variant}")
        list(GET arguments 0 quantum)
        list(GET arguments 1 startup_block_ms)
        list(GET arguments 2 consumer_block_ms)
        list(GET arguments 3 timezone)
        list(GET arguments 4 hash_seed)
        set(environment_args "TZ=${timezone}")
        if(hash_seed STREQUAL "default")
            list(APPEND environment_args "--unset=QT_HASH_SEED")
        else()
            list(APPEND environment_args "QT_HASH_SEED=${hash_seed}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env ${environment_args} --
                    "${TEST_EXECUTABLE}" --determinism-report
                    "${quantum}" "${startup_block_ms}" "${consumer_block_ms}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE digest
            ERROR_VARIABLE error_output
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT result EQUAL 0)
            message(FATAL_ERROR
                "determinism run failed (variant=${variant}, repetition=${repetition}):\n${error_output}")
        endif()
        if(NOT digest MATCHES "^[0-9a-f][0-9a-f]*$")
            message(FATAL_ERROR "determinism run returned an invalid SHA-256 digest: '${digest}'")
        endif()
        string(LENGTH "${digest}" digest_length)
        if(NOT digest_length EQUAL 64)
            message(FATAL_ERROR "determinism digest has length ${digest_length}, expected 64")
        endif()
        if(reference STREQUAL "")
            set(reference "${digest}")
        elseif(NOT digest STREQUAL reference)
            message(FATAL_ERROR
                "non-deterministic result: expected ${reference}, got ${digest} "
                "(variant=${variant}, repetition=${repetition})")
        endif()
    endforeach()
endforeach()

if(NOT reference STREQUAL expected_digest)
    message(FATAL_ERROR "canonical digest changed: expected ${expected_digest}, got ${reference}")
endif()

message(STATUS "deterministic simulation digest: ${reference}")
