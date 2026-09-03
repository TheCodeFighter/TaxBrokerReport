# Define the core library
add_library(taxbroker_core STATIC
    generators/dho_generator.cpp
    generators/div_generator.cpp
    generators/kdvp_generator.cpp
    generators/xml_generator.cpp
    parsers/ibkr_parser.cpp
    parsers/parser_factory.cpp
    parsers/traderepublic_parser.cpp
    processors/fifo_matcher.cpp
    processors/report_processor.cpp
    processors/tax_processor.cpp
    utils/date_utils.cpp
    utils/logger.cpp
    utils/numeric_util.cpp
    utils/string_utils.cpp
    utils/thread_pool.cpp
)

target_include_directories(taxbroker_core PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
    ${CSV_PARSER_INCLUDE_DIR}
)

target_link_libraries(taxbroker_core
    PUBLIC
    spdlog::spdlog
)

# JSON is an API/presentation concern. Keeping it in a separate target prevents the parser and
# tax-processing core from depending on a wire format.
add_library(taxbroker_api STATIC
    server/api/diagnostics_json.cpp
)

target_include_directories(taxbroker_api
    PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(taxbroker_api
    PRIVATE
    nlohmann_json::nlohmann_json
)

# Server executable
add_executable(taxbroker_server
    main.cpp
    server/server.cpp
    server/api/health_api.cpp
    server/api/report_api.cpp
)

target_include_directories(taxbroker_server PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(taxbroker_server PRIVATE
    taxbroker_core
    taxbroker_api
)
