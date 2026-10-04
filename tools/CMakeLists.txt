add_executable(taxbroker_tr_dump
    tr_parser_dump.cpp
)

target_link_libraries(taxbroker_tr_dump PRIVATE
    taxbroker_core
    taxbroker_api
)

add_executable(taxbroker_exact_dump exact_arithmetic_dump.cpp)
target_link_libraries(taxbroker_exact_dump PRIVATE taxbroker_core)

add_test(NAME ExactArithmeticDump.LocalTransactions
    COMMAND ${CMAKE_COMMAND}
        -DDUMP_TOOL=$<TARGET_FILE:taxbroker_exact_dump>
        -DFIXTURE_DIR=${CMAKE_SOURCE_DIR}/tests/test_data/csv
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-output
        -P ${CMAKE_SOURCE_DIR}/tests/support/check_exact_dump.cmake
)
set_tests_properties(ExactArithmeticDump.LocalTransactions PROPERTIES LABELS debug)
