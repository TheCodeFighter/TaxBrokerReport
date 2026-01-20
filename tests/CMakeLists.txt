# Test Configuration
find_package(GTest REQUIRED)

set(TEST_LIBS GTest::gtest GTest::gtest_main)

# Unit Tests Helper
function(add_edavki_test TEST_NAME SOURCE_FILE)
    add_executable(${TEST_NAME} 
        ${SOURCE_FILE} 
        helper.cpp 
        ${CORE_OBJECTS}
    )
    
    target_include_directories(${TEST_NAME} PRIVATE ${EDAVKI_INCLUDES})
    
    target_compile_definitions(${TEST_NAME} PRIVATE 
        PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )

    target_link_libraries(${TEST_NAME} PRIVATE 
        CoreLib 
        ${TEST_LIBS}
    )
    
    gtest_discover_tests(${TEST_NAME})
endfunction()

# Unit Tests
add_edavki_test(test_report_loader test_report_loader.cpp)
add_edavki_test(test_xml_generator test_xml_generator.cpp)
add_edavki_test(test_application_service test_application_service.cpp)

# GUI Tests (Qt Dependent)
add_executable(test_gui 
    test_gui.cpp
    ${PROJECT_SOURCE_DIR}/include/gui/main_window.hpp
    ${PROJECT_SOURCE_DIR}/include/gui/worker.hpp
    ${PROJECT_SOURCE_DIR}/src/gui/main_window.cpp 
    ${PROJECT_SOURCE_DIR}/src/gui/worker.cpp
    ${PROJECT_SOURCE_DIR}/resources/gui/resources.qrc
    ${CORE_OBJECTS}
)

target_include_directories(test_gui PRIVATE ${EDAVKI_INCLUDES})

target_link_libraries(test_gui PRIVATE 
    CoreLib 
    ${TEST_LIBS}
    ${QT_LIBS} 
)

gtest_discover_tests(test_gui)