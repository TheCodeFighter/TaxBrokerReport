include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.18.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# libxml2 is used only by the tests, to check the XML against the FURS schemas. The Docker image
# always has it. A native build without it still builds and runs every test; the schema checks
# then show as skipped (see CONTRIBUTING.md).
find_package(LibXml2)

if(NOT LibXml2_FOUND)
    message(WARNING "libxml2 not found: the XSD validation tests are skipped. "
                    "See CONTRIBUTING.md to install it.")
endif()

# Unit Tests
add_executable(taxbroker_unit_tests
    unit/capital_gains_report_builder_test.cpp
    unit/event_metadata_test.cpp
    unit/exact_arithmetic_test.cpp
    unit/import_fixed_point_test.cpp
    unit/fifo_basis_test.cpp
    unit/fifo_matcher_test.cpp
    unit/processing_diagnostics_test.cpp
    unit/date_utils_test.cpp
    unit/diagnostic_messages_test.cpp
    unit/diagnostics_json_test.cpp
    unit/kdvp_xml_writer_test.cpp
    unit/statement_merger_test.cpp
    unit/tax_processor_test.cpp
    unit/traderepublic_parser_test.cpp
    unit/numeric_util_test.cpp
)

target_include_directories(taxbroker_unit_tests PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(taxbroker_unit_tests
    PRIVATE
    taxbroker_core
    taxbroker_api
    taxbroker_xml
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)

target_compile_definitions(taxbroker_unit_tests
    PRIVATE
    TAXBROKER_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/resources/xml/schemas"
)

if(LibXml2_FOUND)
    target_link_libraries(taxbroker_unit_tests PRIVATE LibXml2::LibXml2)
    target_compile_definitions(taxbroker_unit_tests PRIVATE TAXBROKER_HAS_LIBXML2)
endif()

gtest_discover_tests(taxbroker_unit_tests PROPERTIES LABELS unit)

# Integration Tests
add_executable(taxbroker_integration_tests
    integration/fifo_matching_integration_test.cpp
    integration/reporting_year_integration_test.cpp
    integration/unit_evidence_integration_test.cpp
    integration/statement_merger_integration_test.cpp
)

target_include_directories(taxbroker_integration_tests PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(taxbroker_integration_tests
    PRIVATE
    taxbroker_core
    GTest::gtest_main
)

gtest_discover_tests(taxbroker_integration_tests PROPERTIES LABELS integration)
