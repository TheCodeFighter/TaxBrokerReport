# Text files are embedded in the library, so the application needs no file at run time. A change to
# a file re-runs CMake and rebuilds its generated source. The text is written as a byte array
# ('\x6b','\x65',...), not a string literal: MSVC limits a string literal to about 16 KB, and a byte
# array has no such limit on any compiler.
# embed_text_file(<file> <function> <header declaring it> <generated source>)
function(embed_text_file aFile aFunction aHeader aOutput)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${aFile})
    file(READ ${aFile} EMBED_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1'," EMBED_BYTES "${EMBED_HEX}")
    file(RELATIVE_PATH EMBED_FILE_NAME ${CMAKE_SOURCE_DIR} ${aFile})
    set(EMBED_FUNCTION ${aFunction})
    set(EMBED_HEADER ${aHeader})
    configure_file(utils/embedded_text.cpp.in ${aOutput} @ONLY)
endfunction()

set(EMBEDDED_DIR ${CMAKE_CURRENT_BINARY_DIR}/embedded)
set(RESOURCES_DIR ${CMAKE_SOURCE_DIR}/resources)
embed_text_file(${RESOURCES_DIR}/i18n/diagnostic_messages.csv diagnosticMessagesCsv
    taxbroker/diagnostic_messages.hpp ${EMBEDDED_DIR}/diagnostic_messages_table.cpp)
embed_text_file(${RESOURCES_DIR}/reference/payers.csv embeddedPayersCsv
    taxbroker/reference_catalogue.hpp ${EMBEDDED_DIR}/payers_csv.cpp)
embed_text_file(${RESOURCES_DIR}/reference/relief_statements.csv embeddedReliefStatementsCsv
    taxbroker/reference_catalogue.hpp ${EMBEDDED_DIR}/relief_statements_csv.cpp)
embed_text_file(${RESOURCES_DIR}/reference/broker_payers.csv embeddedBrokerPayersCsv
    taxbroker/reference_catalogue.hpp ${EMBEDDED_DIR}/broker_payers_csv.cpp)

# The compact ECB rate files of the calendar years built into the application, one file
# resources/ecb/rates_<year>.csv per year. A new year is a new file and a new entry here.
set(BUILT_IN_ECB_YEARS 2016 2017 2018 2019 2020 2021 2022 2023 2024 2025)
set(ECB_ARRAYS "")
set(ECB_TABLE "")
list(LENGTH BUILT_IN_ECB_YEARS ECB_COUNT)
foreach(ECB_YEAR ${BUILT_IN_ECB_YEARS})
    set(ECB_FILE ${RESOURCES_DIR}/ecb/rates_${ECB_YEAR}.csv)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ECB_FILE})
    file(READ ${ECB_FILE} ECB_HEX HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1'," ECB_BYTES "${ECB_HEX}")
    string(APPEND ECB_ARRAYS "constexpr char kRates${ECB_YEAR}[] = {${ECB_BYTES}};\n\n")
    string(APPEND ECB_TABLE
        "        BuiltInRateFile{${ECB_YEAR}, \"rates_${ECB_YEAR}.csv\", "
        "{kRates${ECB_YEAR}, sizeof(kRates${ECB_YEAR})}},\n")
endforeach()
configure_file(utils/built_in_ecb_rates.cpp.in ${EMBEDDED_DIR}/built_in_ecb_rates.cpp @ONLY)

# Define the core library
add_library(taxbroker_core STATIC
    parsers/ecb_rate_file.cpp
    parsers/ibkr_parser.cpp
    parsers/reference_catalogue.cpp
    parsers/traderepublic_parser.cpp
    processors/application_result.cpp
    processors/bond_years.cpp
    processors/failure_resolution_stage.cpp
    processors/capital_gains_report_builder.cpp
    processors/capital_report_gates.cpp
    processors/corporate_actions.cpp
    processors/decision_session.cpp
    processors/dividend_payments.cpp
    processors/fifo_basis.cpp
    processors/fifo_matcher.cpp
    processors/income_classifier.cpp
    processors/income_payers.cpp
    processors/income_reports.cpp
    processors/interest_payments.cpp
    processors/loss_eligibility.cpp
    processors/official_rate_selection.cpp
    processors/processing_diagnostics.cpp
    processors/quantity_reconciliation.cpp
    processors/statement_merger.cpp
    processors/tax_classifier.cpp
    processors/tax_processor.cpp
    processors/trade_conversion.cpp
    ${EMBEDDED_DIR}/broker_payers_csv.cpp
    ${EMBEDDED_DIR}/built_in_ecb_rates.cpp
    ${EMBEDDED_DIR}/diagnostic_messages_table.cpp
    ${EMBEDDED_DIR}/payers_csv.cpp
    ${EMBEDDED_DIR}/relief_statements_csv.cpp
    utils/csv_text_reader.cpp
    utils/currency.cpp
    utils/date_utils.cpp
    utils/diagnostic_messages.cpp
    utils/exact_arithmetic.cpp
    utils/logger.cpp
    utils/numeric_util.cpp
    utils/split_ratio.cpp
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

# Keeping XML in a separate target keeps pugixml out of
# the parser and tax-processing core.
add_library(taxbroker_xml STATIC
    xml/dho_xml_writer.cpp
    xml/div_xml_writer.cpp
    xml/kdvp_xml_writer.cpp
    xml/xml_common.cpp
)

target_include_directories(taxbroker_xml
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(taxbroker_xml
    PUBLIC
    taxbroker_core
    PRIVATE
    pugixml::static
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

# The command line: the report command of the walking skeleton. The library holds the whole
# command so the tests can run it in process; the executable only sets up logging.
add_library(taxbroker_cli STATIC
    cli/decision_prompts.cpp
    cli/report_command.cpp
    cli/taxpayer_file.cpp
)

target_include_directories(taxbroker_cli
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(taxbroker_cli
    PUBLIC
    taxbroker_core
    PRIVATE
    taxbroker_api
    taxbroker_xml
    nlohmann_json::nlohmann_json
)

add_executable(taxbroker
    cli/main.cpp
)

target_link_libraries(taxbroker PRIVATE
    taxbroker_cli
)

# The production build (docker/Dockerfile) links both executables fully static: the Alpine build
# uses musl, so a dynamic binary would not run on other Linux systems, and a static one runs on any
# x86-64 Linux. The development, coverage and test builds stay dynamic.
option(TAXBROKER_STATIC "Link taxbroker and taxbroker_server fully static" OFF)

if(TAXBROKER_STATIC)
    target_link_options(taxbroker PRIVATE -static)
    target_link_options(taxbroker_server PRIVATE -static)
endif()
