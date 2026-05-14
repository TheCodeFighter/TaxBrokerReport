include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Unit Tests
add_executable(taxbroker_unit_tests
    unit/fifo_matcher_test.cpp
    unit/ibkr_parser_test.cpp
    unit/tax_processor_test.cpp
    unit/traderepublic_parser_test.cpp
    unit/xml_generator_test.cpp
)

target_include_directories(taxbroker_unit_tests PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(taxbroker_unit_tests
    PRIVATE
    taxbroker_core
    GTest::gtest_main
)

gtest_discover_tests(taxbroker_unit_tests)

# Integration Tests
add_executable(taxbroker_integration_tests
    integration/full_pipeline_test.cpp
    integration/server_integration_test.cpp
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

gtest_discover_tests(taxbroker_integration_tests)
