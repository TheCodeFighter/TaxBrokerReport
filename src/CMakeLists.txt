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
    parsers/reference_catalogue.hpp ${EMBEDDED_DIR}/payers_csv.cpp)
embed_text_file(${RESOURCES_DIR}/reference/relief_statements.csv embeddedReliefStatementsCsv
    parsers/reference_catalogue.hpp ${EMBEDDED_DIR}/relief_statements_csv.cpp)
embed_text_file(${RESOURCES_DIR}/reference/broker_payers.csv embeddedBrokerPayersCsv
    parsers/reference_catalogue.hpp ${EMBEDDED_DIR}/broker_payers_csv.cpp)

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
    parsers/csv_parser.cpp
    parsers/ecb_rate_file.cpp
    parsers/input_file.cpp
    parsers/reference_catalogue.cpp
    parsers/traderepublic_parser.cpp
    processors/application_result.cpp
    processors/application_summaries.cpp
    processors/bond_years.cpp
    processors/developer_export_stage.cpp
    processors/failure_resolution_stage.cpp
    processors/fee_summary.cpp
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
    utils/build_info.cpp
    utils/csv_text_reader.cpp
    utils/currency.cpp
    utils/date_utils.cpp
    utils/diagnostic_messages.cpp
    utils/exception_kind.cpp
    utils/exact_arithmetic.cpp
    utils/logger.cpp
    utils/numeric_util.cpp
    utils/split_ratio.cpp
    utils/state_folder.cpp
    utils/tax_number.cpp
    utils/xml_text.cpp
)

# Only include/ is public. The headers of the stages, parsers, writers and utilities are in src/
# and are private to the backend libraries, so a consumer reaches the backend through the
# application service.
target_include_directories(taxbroker_core
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CSV_PARSER_INCLUDE_DIR}
)

# The version of the program, for the start line of the log and GET /api/version.
target_compile_definitions(taxbroker_core PRIVATE TAXBROKER_VERSION="${PROJECT_VERSION}")

target_link_libraries(taxbroker_core
    PUBLIC
    Boost::headers
    spdlog::spdlog
)

# JSON is an API/presentation concern. Keeping it in a separate target prevents the parser and
# tax-processing core from depending on a wire format.
add_library(taxbroker_api STATIC
    server/api/answer_json.cpp
    server/api/application_result_json.cpp
    server/api/diagnostics_json.cpp
    server/api/json_strings.cpp
    server/api/messages_json.cpp
    server/api/report_files.cpp
    server/api/request_json.cpp
    server/api/support_report_json.cpp
    server/api/taxpayer_json.cpp
)

target_include_directories(taxbroker_api
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
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
    xml/obr_xml_writer.cpp
    xml/report_xml_stage.cpp
    xml/xml_common.cpp
)

target_include_directories(taxbroker_xml
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(taxbroker_xml
    PUBLIC
    taxbroker_core
    PRIVATE
    pugixml::static
)

# The Interactive Brokers parser reads XML, so it is its own target and pugixml stays out of the
# core. The service links it with the parser of each other broker.
add_library(taxbroker_ibkr STATIC
    parsers/ibkr_parser.cpp
    parsers/ibkr_time.cpp
)

target_include_directories(taxbroker_ibkr
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(taxbroker_ibkr
    PUBLIC
    taxbroker_core
    PRIVATE
    pugixml::static
)

# The application service: one run parses, merges and calculates every report and writes the XML.
# It lives outside the core because the XML writers (pugixml) link the core.
add_library(taxbroker_service STATIC
    service/application_run.cpp
)

target_include_directories(taxbroker_service
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# The core is an implementation detail of the service (a static library still links it into the
# consumer). Boost (exact arithmetic header) and spdlog (logging macros) are in public headers.
target_link_libraries(taxbroker_service
    PUBLIC
    Boost::headers
    spdlog::spdlog
    PRIVATE
    taxbroker_core
    taxbroker_ibkr
    taxbroker_xml
)

# The checks and files of the local server login that need no HTTP library: loopback Host and
# Origin, tokens, session cookie, security headers and the private launch file.
add_library(taxbroker_server_security STATIC
    server/request_guard.cpp
    server/launch_file.cpp
)

if(WIN32)
    target_link_libraries(taxbroker_server_security PRIVATE bcrypt)
endif()

# The local HTTP server: options, folders, browser launch, routes and the Beast server. It reaches
# the backend only through the public headers (the service); the logger comes with the service.
add_library(taxbroker_server_lib STATIC
    server/browser_open.cpp
    server/http_message.cpp
    server/http_server.cpp
    server/log_level_routes.cpp
    server/multipart.cpp
    server/profile_routes.cpp
    server/profile_store.cpp
    server/routes.cpp
    server/run_routes.cpp
    server/server.cpp
    server/server_config.cpp
    server/state_folder.cpp
    server/static_files.cpp
)

# The files of the built frontend (frontend/, scripts/frontend.sh) are embedded in the server, so
# the application needs no file at run time. Without TAXBROKER_FRONTEND_DIR the server serves only
# a placeholder page, so the C++ builds and tests need no Node.
set(TAXBROKER_FRONTEND_DIR "" CACHE PATH
    "Folder with the built frontend (index.html and assets/) to embed in taxbroker_server")

if(TAXBROKER_FRONTEND_DIR)
    get_filename_component(FRONTEND_DIR ${TAXBROKER_FRONTEND_DIR} ABSOLUTE)
    set(FRONTEND_FILES index.html assets/app.js assets/app.css assets/logo.webp)
    set(FRONTEND_ARRAYS "")
    set(FRONTEND_TABLE "")
    set(FRONTEND_COUNT 0)

    foreach(FRONTEND_FILE ${FRONTEND_FILES})
        set(FRONTEND_PATH ${FRONTEND_DIR}/${FRONTEND_FILE})

        if(NOT EXISTS ${FRONTEND_PATH})
            message(FATAL_ERROR
                "TAXBROKER_FRONTEND_DIR has no ${FRONTEND_FILE}; build the frontend first "
                "(scripts/frontend.sh build)")
        endif()

        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FRONTEND_PATH})
        file(READ ${FRONTEND_PATH} FRONTEND_HEX HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "'\\\\x\\1'," FRONTEND_BYTES "${FRONTEND_HEX}")
        string(APPEND FRONTEND_ARRAYS
            "constexpr char kFrontendFile${FRONTEND_COUNT}[] = {${FRONTEND_BYTES}};\n\n")
        string(APPEND FRONTEND_TABLE
            "    StaticFile{\"${FRONTEND_FILE}\", {kFrontendFile${FRONTEND_COUNT}, "
            "sizeof(kFrontendFile${FRONTEND_COUNT})}},\n")
        math(EXPR FRONTEND_COUNT "${FRONTEND_COUNT} + 1")
    endforeach()

    configure_file(server/embedded_frontend.cpp.in ${EMBEDDED_DIR}/embedded_frontend.cpp @ONLY)
    target_sources(taxbroker_server_lib PRIVATE ${EMBEDDED_DIR}/embedded_frontend.cpp)
    target_compile_definitions(taxbroker_server_lib PRIVATE TAXBROKER_EMBED_FRONTEND)
    target_include_directories(taxbroker_server_lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
endif()

find_package(Threads REQUIRED)

target_link_libraries(taxbroker_server_lib
    PUBLIC
    taxbroker_service
    PRIVATE
    taxbroker_api
    taxbroker_server_security
    nlohmann_json::nlohmann_json
    Threads::Threads
)

# Server executable
add_executable(taxbroker_server
    server/main.cpp
)

target_link_libraries(taxbroker_server PRIVATE
    taxbroker_server_lib
    taxbroker_api
)

# The command line: the report command of the walking skeleton. The library holds the whole
# command so the tests can run it in process; the executable only sets up logging.
add_library(taxbroker_cli STATIC
    cli/decision_prompts.cpp
    cli/output_files.cpp
    cli/report_command.cpp
    cli/taxpayer_file.cpp
)

# The CLI sees its own headers (src/cli) and the public headers it gets from the service; it
# cannot include a stage, parser, writer or utility header.
target_include_directories(taxbroker_cli
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src/cli
)

target_link_libraries(taxbroker_cli
    PUBLIC
    taxbroker_service
    PRIVATE
    taxbroker_api
    nlohmann_json::nlohmann_json
)

add_executable(taxbroker
    cli/main.cpp
)

target_link_libraries(taxbroker PRIVATE
    taxbroker_cli
)

# Every public header must compile alone, with only include/ and the third-party headers the
# public headers name (Boost for the exact arithmetic, spdlog for the logging macros). A public
# header that needs a src/ header fails this check. The files are generated, one per header, and
# the objects are not linked into anything.
file(GLOB_RECURSE PUBLIC_HEADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/include/taxbroker/*.hpp)
set(PUBLIC_HEADER_CHECK_SOURCES "")

foreach(PUBLIC_HEADER ${PUBLIC_HEADERS})
    file(RELATIVE_PATH PUBLIC_HEADER_PATH ${CMAKE_SOURCE_DIR}/include ${PUBLIC_HEADER})
    string(REPLACE "/" "_" PUBLIC_HEADER_NAME ${PUBLIC_HEADER_PATH})
    set(PUBLIC_HEADER_CHECK_FILE
        ${CMAKE_CURRENT_BINARY_DIR}/public_header_check/${PUBLIC_HEADER_NAME}.cpp)
    file(GENERATE OUTPUT ${PUBLIC_HEADER_CHECK_FILE}
        CONTENT "#include \"${PUBLIC_HEADER_PATH}\"\n")
    list(APPEND PUBLIC_HEADER_CHECK_SOURCES ${PUBLIC_HEADER_CHECK_FILE})
endforeach()

add_library(taxbroker_public_header_check OBJECT ${PUBLIC_HEADER_CHECK_SOURCES})

target_include_directories(taxbroker_public_header_check PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(taxbroker_public_header_check PRIVATE
    Boost::headers
    spdlog::spdlog
)

# The production build (docker/Dockerfile) links both executables fully static: the Alpine build
# uses musl, so a dynamic binary would not run on other Linux systems, and a static one runs on any
# x86-64 Linux. The development, coverage and test builds stay dynamic.
option(TAXBROKER_STATIC "Link taxbroker and taxbroker_server fully static" OFF)

if(TAXBROKER_STATIC)
    target_link_options(taxbroker PRIVATE -static)
    target_link_options(taxbroker_server PRIVATE -static)
endif()
