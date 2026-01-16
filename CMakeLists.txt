cmake_minimum_required(VERSION 3.20)
project(EdavkiXmlMaker VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------
# 1. Performance Optimization (CCache)
# -----------------------------
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message(STATUS "Using ccache for faster builds")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# -----------------------------
# 2. Build Strategy (Debug vs Release)
# -----------------------------
string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPER)

if(CMAKE_BUILD_TYPE_UPPER STREQUAL "DEBUG")
    message(STATUS "Build Mode: DEBUG (Optimized for speed)")
    set(CORE_LIB_TYPE OBJECT)
    add_compile_definitions(UNIT_TEST)
    set(CORE_OBJECTS $<TARGET_OBJECTS:CoreLib>)
else()
    message(STATUS "Build Mode: RELEASE (Optimized for distribution)")
    set(CORE_LIB_TYPE STATIC)
    set(CORE_OBJECTS "") # no need to add objects manually
endif()

# -----------------------------
# Dependencies
# -----------------------------
find_package(Threads REQUIRED)
find_package(GTest REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(POPPLER REQUIRED IMPORTED_TARGET poppler-cpp)
pkg_check_modules(PUGIXML REQUIRED IMPORTED_TARGET pugixml)
pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)

# -----------------------------
# Core Library
# -----------------------------
add_library(CoreLib ${CORE_LIB_TYPE}
    src/backend/report_loader.cpp
    src/backend/xml_generator.cpp
    src/app/application_service.cpp
    src/platform_config.cpp
    util/util_xml.cpp
    util/config.cpp
)

target_include_directories(CoreLib 
    PUBLIC 
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include/app>
    PRIVATE 
        ${CMAKE_SOURCE_DIR}/include/backend
        ${CMAKE_SOURCE_DIR}/util
)

target_link_libraries(CoreLib PUBLIC 
    nlohmann_json::nlohmann_json
    PkgConfig::PUGIXML
    PkgConfig::POPPLER
    PkgConfig::LIBXML2
    Threads::Threads
)

# -----------------------------
# Main Application
# -----------------------------
add_executable(EdavkiXmlMaker src/main.cpp ${CORE_OBJECTS})

target_link_libraries(EdavkiXmlMaker PRIVATE CoreLib)

# -----------------------------
# Testing Setup
# -----------------------------
enable_testing()
include(GoogleTest)

function(add_edavki_test TEST_NAME SOURCE_FILE)
    add_executable(${TEST_NAME} ${SOURCE_FILE} tests/helper.cpp ${CORE_OBJECTS})
    
    target_link_libraries(${TEST_NAME} PRIVATE 
        CoreLib 
        GTest::gtest 
        GTest::gtest_main
    )

    target_include_directories(${TEST_NAME} PRIVATE 
        ${CMAKE_SOURCE_DIR}/util 
        ${CMAKE_SOURCE_DIR}/include/backend
    )

    target_compile_definitions(${TEST_NAME} PRIVATE 
        PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    
    gtest_discover_tests(${TEST_NAME})
endfunction()

add_edavki_test(test_report_loader tests/test_report_loader.cpp)
add_edavki_test(test_xml_generator tests/test_xml_generator.cpp)
add_edavki_test(test_application_service tests/test_application_service.cpp)