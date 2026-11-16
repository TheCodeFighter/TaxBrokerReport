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
    support/large_allocation_hook.cpp
    unit/answer_json_test.cpp
    unit/application_result_json_test.cpp
    unit/application_result_test.cpp
    unit/application_summaries_test.cpp
    unit/failure_resolution_stage_test.cpp
    unit/fee_summary_test.cpp
    unit/capital_gains_report_builder_test.cpp
    unit/capital_report_gates_test.cpp
    unit/corporate_actions_test.cpp
    unit/decision_prompts_test.cpp
    unit/decision_session_test.cpp
    unit/event_metadata_test.cpp
    unit/exact_arithmetic_test.cpp
    unit/import_fixed_point_test.cpp
    unit/income_report_models_test.cpp
    unit/fifo_basis_property_test.cpp
    unit/fifo_basis_test.cpp
    unit/fifo_matcher_property_test.cpp
    unit/fifo_matcher_test.cpp
    unit/loss_eligibility_test.cpp
    unit/processing_diagnostics_test.cpp
    unit/output_files_test.cpp
    unit/report_command_test.cpp
    unit/currency_test.cpp
    unit/date_utils_test.cpp
    unit/diagnostic_messages_test.cpp
    unit/diagnostics_json_test.cpp
    unit/dividend_failure_test.cpp
    unit/dividend_payments_test.cpp
    unit/dho_xml_writer_test.cpp
    unit/obr_xml_writer_test.cpp
    unit/report_xml_stage_test.cpp
    unit/div_xml_writer_test.cpp
    unit/income_classifier_test.cpp
    unit/income_payers_test.cpp
    unit/interest_payments_test.cpp
    unit/other_interest_failure_test.cpp
    unit/ecb_rate_file_test.cpp
    unit/input_file_test.cpp
    unit/official_rate_selection_test.cpp
    unit/quantity_reconciliation_test.cpp
    unit/browser_open_test.cpp
    unit/launch_file_test.cpp
    unit/logger_settings_test.cpp
    unit/exception_kind_test.cpp
    unit/multipart_test.cpp
    unit/request_guard_test.cpp
    unit/server_config_test.cpp
    unit/messages_json_test.cpp
    unit/server_routes_test.cpp
    unit/server_static_files_test.cpp
    unit/state_folder_test.cpp
    unit/reference_catalogue_test.cpp
    unit/rejection_json_test.cpp
    unit/run_routes_test.cpp
    unit/report_file_name_test.cpp
    unit/request_json_test.cpp
    unit/kdvp_xml_writer_test.cpp
    unit/statement_merger_property_test.cpp
    unit/statement_merger_test.cpp
    unit/left_out_instruments_test.cpp
    unit/tax_classifier_test.cpp
    unit/tax_processor_test.cpp
    unit/taxpayer_file_test.cpp
    unit/taxpayer_json_test.cpp
    unit/profile_json_test.cpp
    unit/profile_routes_test.cpp
    unit/profile_store_test.cpp
    unit/trade_conversion_test.cpp
    unit/trade_republic_parser_checks_test.cpp
    unit/traderepublic_parser_test.cpp
    unit/csv_text_reader_test.cpp
    unit/numeric_util_test.cpp
    unit/obr_schema_test.cpp
    unit/split_ratio_test.cpp
    unit/tax_number_test.cpp
)

target_include_directories(taxbroker_unit_tests PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CSV_PARSER_INCLUDE_DIR}
)

target_link_libraries(taxbroker_unit_tests
    PRIVATE
    taxbroker_core
    taxbroker_api
    taxbroker_cli
    taxbroker_service
    taxbroker_server_lib
    taxbroker_server_security
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
    integration/application_result_integration_test.cpp
    integration/application_run_integration_test.cpp
    integration/developer_export_integration_test.cpp
    integration/cli_report_integration_test.cpp
    integration/corporate_action_integration_test.cpp
    integration/ecb_rate_file_integration_test.cpp
    integration/fifo_matching_integration_test.cpp
    integration/full_pipeline_integration_test.cpp
    integration/historical_scenario_integration_test.cpp
    integration/income_reports_integration_test.cpp
    integration/income_xml_integration_test.cpp
    integration/kdvp_xml_integration_test.cpp
    integration/loss_eligibility_integration_test.cpp
    integration/reporting_year_integration_test.cpp
    integration/server_run_test.cpp
    integration/server_test.cpp
    integration/unit_evidence_integration_test.cpp
    integration/statement_merger_integration_test.cpp
)

target_include_directories(taxbroker_integration_tests PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CSV_PARSER_INCLUDE_DIR}
)

target_link_libraries(taxbroker_integration_tests
    PRIVATE
    taxbroker_core
    taxbroker_api
    taxbroker_cli
    taxbroker_server_lib
    taxbroker_service
    taxbroker_xml
    GTest::gtest_main
    nlohmann_json::nlohmann_json
    pugixml::static
)

target_compile_definitions(taxbroker_integration_tests
    PRIVATE
    TAXBROKER_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/resources/xml/schemas"
)

if(LibXml2_FOUND)
    target_link_libraries(taxbroker_integration_tests PRIVATE LibXml2::LibXml2)
    target_compile_definitions(taxbroker_integration_tests PRIVATE TAXBROKER_HAS_LIBXML2)
endif()

gtest_discover_tests(taxbroker_integration_tests PROPERTIES LABELS integration)

# The real taxbroker executable on the synthetic CLI fixture, against a golden XML file.
add_test(NAME CliReport.Binary
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:taxbroker>
        -DFIXTURE_DIR=${CMAKE_SOURCE_DIR}/tests/test_data/cli
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-report-output
        -P ${CMAKE_SOURCE_DIR}/tests/support/check_cli_report.cmake
)
set_tests_properties(CliReport.Binary PROPERTIES LABELS integration)

# The public API boundary. Each probe is one file that includes an internal header, built with the
# include directories and libraries of the real target (read from it, so they cannot drift). The
# probe is not part of the normal build; the test builds it and passes only when the compiler
# reports that very header as missing, so a build that fails for another reason does not pass.
# The label is boundary, not unit: the test runs cmake --build, which would rebuild the backend
# from a restored build tree in CI, so CI runs it right after the build (see ci.yml).
# One lock: two builds in the same build folder must not run at the same time.
set(BOUNDARY_PROBES
    "CliCannotIncludeInternal|taxbroker_cli|cli_includes_internal|fifo_matcher"
    "ServerCannotIncludeInternal|taxbroker_server|server_includes_internal|traderepublic_parser"
)

foreach(PROBE ${BOUNDARY_PROBES})
    string(REPLACE "|" ";" PROBE_PARTS ${PROBE})
    list(GET PROBE_PARTS 0 PROBE_TEST)
    list(GET PROBE_PARTS 1 PROBE_TARGET)
    list(GET PROBE_PARTS 2 PROBE_SOURCE)
    list(GET PROBE_PARTS 3 PROBE_HEADER)
    set(PROBE_NAME boundary_${PROBE_SOURCE})
    # The "header missing" text of Clang, GCC and MSVC; the test passes when one matches.
    set(PROBE_PATTERNS
        "${PROBE_HEADER}\\.hpp'? file not found"
        "${PROBE_HEADER}\\.hpp: No such file"
        "Cannot open include file: '[a-z]+/${PROBE_HEADER}"
    )

    add_library(${PROBE_NAME} OBJECT EXCLUDE_FROM_ALL boundary/${PROBE_SOURCE}.cpp)
    target_include_directories(${PROBE_NAME} PRIVATE
        "$<TARGET_PROPERTY:${PROBE_TARGET},INCLUDE_DIRECTORIES>")
    target_link_libraries(${PROBE_NAME} PRIVATE
        "$<TARGET_PROPERTY:${PROBE_TARGET},LINK_LIBRARIES>")

    add_test(NAME PublicApiBoundary.${PROBE_TEST}
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${PROBE_NAME})
    set_tests_properties(PublicApiBoundary.${PROBE_TEST} PROPERTIES
        LABELS boundary
        RESOURCE_LOCK boundary_build
        PASS_REGULAR_EXPRESSION "${PROBE_PATTERNS}")
endforeach()
