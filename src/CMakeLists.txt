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
    utils/string_utils.cpp
    utils/thread_pool.cpp
)

target_include_directories(taxbroker_core PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(taxbroker_core
    PUBLIC
    spdlog::spdlog
)

# Server executable
add_executable(taxbroker_server
    server/server.cpp
    server/api/health_api.cpp
    server/api/report_api.cpp
)

target_include_directories(taxbroker_server PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(taxbroker_server PRIVATE
    taxbroker_core
)