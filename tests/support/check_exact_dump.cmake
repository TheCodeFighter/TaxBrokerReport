# Exercise the opt-in debug executable with fictional data only.
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(output "${OUTPUT_DIR}/exact_dump_check.txt")

function(run_dump fixture)
    execute_process(COMMAND "${DUMP_TOOL}" "${fixture}" "${output}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Debug dump failed: ${error}")
    endif()
    file(READ "${output}" contents)
    set(contents "${contents}" PARENT_SCOPE)
endfunction()

function(require_text text)
    string(FIND "${contents}" "${text}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Missing expected transaction result: ${text}")
    endif()
endfunction()

run_dump("${FIXTURE_DIR}/exact_dump_open_split_fixture.csv" --split-row 3 2 1)
require_text("Units before split: 2.00000000")
require_text("Units after split: 4.00000000")
require_text("Purchase cost EUR (8-decimal display): 24.69120000")
require_text("Purchase cost per unit EUR (8-decimal display): 6.17280000")
require_text("Data guess if exported quantity means ADDED units: 2 new shares / 1 old shares")
require_text("Imaginary comparison (new/old = 1/2; quantity halved): 1 new shares / 2 old shares")
string(FIND "${contents}" "Event: Sale" invented_sale)
if(NOT invented_sale EQUAL -1)
    message(FATAL_ERROR "An unsold position must not contain an invented sale")
endif()

run_dump("${FIXTURE_DIR}/exact_dump_open_split_fixture.csv")
require_text("Real split factor: not verified from this CSV")
require_text("Units and price after this action are unresolved.")

run_dump("${FIXTURE_DIR}/traderepublic_parser_supported_fixture.csv")
# The real sale in this fictional fixture must affect the split's opening position,
# but neither that sale nor instruments without actions belong in this dump.
require_text("Units before split: 1.00000000")
foreach(forbidden "Event: Sale" "Event: Purchase" "Synthetic Index Fund" "Position at end")
    string(FIND "${contents}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "Split dump includes unrelated output: ${forbidden}")
    endif()
endforeach()

# A wrong row must fail before replacing an existing inspection file.
file(WRITE "${output}" "keep existing output")
execute_process(COMMAND "${DUMP_TOOL}" "${FIXTURE_DIR}/exact_dump_open_split_fixture.csv"
    "${output}" --split-row 99 2 1 RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
file(READ "${output}" contents)
if(result EQUAL 0 OR NOT contents STREQUAL "keep existing output")
    message(FATAL_ERROR "Invalid action row must fail without replacing output")
endif()
file(REMOVE "${output}")
