# Runs the built taxbroker executable on the synthetic CLI fixture and compares the XML with the
# committed golden file. The in-process tests cover the scenarios; this one checks the real binary.
file(REMOVE_RECURSE "${OUTPUT_DIR}")

execute_process(
    COMMAND "${CLI}" report
        --year 2024
        --taxpayer "${FIXTURE_DIR}/taxpayer.json"
        --out "${OUTPUT_DIR}"
        "${FIXTURE_DIR}/report_fixture.csv"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "taxbroker report exited with ${result}: ${error}")
endif()

file(READ "${OUTPUT_DIR}/Doh-KDVP_2024.xml" xml)
file(READ "${FIXTURE_DIR}/Doh-KDVP_2024.golden.xml" golden)

if(NOT xml STREQUAL golden)
    message(FATAL_ERROR "Doh-KDVP_2024.xml differs from Doh-KDVP_2024.golden.xml")
endif()

if(NOT EXISTS "${OUTPUT_DIR}/summary_2024.txt")
    message(FATAL_ERROR "summary_2024.txt was not written")
endif()

string(FIND "${output}" "Capital gains (Doh-KDVP): generated, written to Doh-KDVP_2024.xml" found)

if(found EQUAL -1)
    message(FATAL_ERROR "The summary on stdout has no generated status line")
endif()

# The fixture has no sale in 2023: no data, exit code 0 and no XML.
execute_process(
    COMMAND "${CLI}" report
        --year 2023
        --taxpayer "${FIXTURE_DIR}/taxpayer.json"
        --out "${OUTPUT_DIR}"
        "${FIXTURE_DIR}/report_fixture.csv"
    RESULT_VARIABLE result
    OUTPUT_QUIET
    ERROR_QUIET)

if(NOT result EQUAL 0 OR EXISTS "${OUTPUT_DIR}/Doh-KDVP_2023.xml")
    message(FATAL_ERROR "A year without sales must exit with 0 and write no XML")
endif()

# A missing option is a failure: exit code 2.
execute_process(
    COMMAND "${CLI}" report --year 2024 "${FIXTURE_DIR}/report_fixture.csv"
    RESULT_VARIABLE result
    OUTPUT_QUIET
    ERROR_QUIET)

if(NOT result EQUAL 2)
    message(FATAL_ERROR "A missing --taxpayer must exit with 2, not ${result}")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
