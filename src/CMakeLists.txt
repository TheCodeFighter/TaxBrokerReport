# The diagnostic message table is embedded in the library, so the application needs no file at run
# time. A change to the CSV re-runs CMake and rebuilds the generated source. The table is written as
# a byte array ('\x6b','\x65',...), not a string literal: MSVC limits a string literal to about
# 16 KB, and a byte array has no such limit on any compiler.
set(DIAGNOSTIC_MESSAGES_CSV_FILE ${CMAKE_SOURCE_DIR}/resources/i18n/diagnostic_messages.csv)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DIAGNOSTIC_MESSAGES_CSV_FILE})
file(READ ${DIAGNOSTIC_MESSAGES_CSV_FILE} DIAGNOSTIC_MESSAGES_CSV_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1'," DIAGNOSTIC_MESSAGES_CSV_BYTES
    "${DIAGNOSTIC_MESSAGES_CSV_HEX}"
)
configure_file(utils/diagnostic_messages_table.cpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/diagnostic_messages_table.cpp
    @ONLY
)

# Define the core library
add_library(taxbroker_core STATIC
    parsers/ibkr_parser.cpp
    parsers/traderepublic_parser.cpp
    processors/application_result.cpp
    processors/capital_gains_report_builder.cpp
    processors/fifo_basis.cpp
    processors/fifo_matcher.cpp
    processors/processing_diagnostics.cpp
    processors/statement_merger.cpp
    processors/tax_processor.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/diagnostic_messages_table.cpp
    utils/date_utils.cpp
    utils/diagnostic_messages.cpp
    utils/exact_arithmetic.cpp
    utils/logger.cpp
    utils/numeric_util.cpp
)

target_include_directories(taxbroker_core PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
    ${CSV_PARSER_INCLUDE_DIR}
)

target_link_libraries(taxbroker_core
    PUBLIC
    Boost::headers
    spdlog::spdlog
)

# JSON is an API/presentation concern. Keeping it in a separate target prevents the parser and
# tax-processing core from depending on a wire format.
add_library(taxbroker_api STATIC
    server/api/application_result_json.cpp
    server/api/diagnostics_json.cpp
    server/api/json_strings.cpp
)

target_include_directories(taxbroker_api
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(taxbroker_api
    PRIVATE
    taxbroker_core
    nlohmann_json::nlohmann_json
)

# Server executable
add_executable(taxbroker_server
    main.cpp
    server/server.cpp
)

target_include_directories(taxbroker_server PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(taxbroker_server PRIVATE
    taxbroker_core
    taxbroker_api
)
