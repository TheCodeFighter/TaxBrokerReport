cmake_minimum_required(VERSION 3.20)
project(EdavkiXmlMaker VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt6 Configuration (MOC, UIC, RCC automation)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# 1. Performance (CCache)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message(STATUS "Using ccache")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# 2. Build Strategy
string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPER)
if(CMAKE_BUILD_TYPE_UPPER STREQUAL "DEBUG")
    message(STATUS "Build Mode: DEBUG")
    add_compile_options(--coverage -fno-inline -fno-inline-small-functions -fno-default-inline)
    add_link_options(--coverage)
    add_compile_definitions(UNIT_TEST)
    set(CORE_LIB_TYPE OBJECT)
    set(CORE_OBJECTS $<TARGET_OBJECTS:CoreLib>)
else()
    message(STATUS "Build Mode: RELEASE")
    set(CORE_LIB_TYPE STATIC)
    set(CORE_OBJECTS "")
endif()

# 3. Dependencies
find_package(Threads REQUIRED)
find_package(GTest REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(POPPLER REQUIRED IMPORTED_TARGET poppler-cpp)
pkg_check_modules(PUGIXML REQUIRED IMPORTED_TARGET pugixml)
pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)

# Get headers
set(EDAVKI_INCLUDES 
    include 
    include/app 
    include/backend 
    include/gui     # Dodano za GUI
    util
)

# 4. Core Library
add_library(CoreLib ${CORE_LIB_TYPE}
    src/backend/report_loader.cpp
    src/backend/xml_generator.cpp
    src/app/application_service.cpp
    src/platform_config.cpp
    util/util_xml.cpp
    util/config.cpp
)

target_include_directories(CoreLib PUBLIC ${EDAVKI_INCLUDES})

target_link_libraries(CoreLib PUBLIC 
    nlohmann_json::nlohmann_json
    PkgConfig::PUGIXML
    PkgConfig::POPPLER
    PkgConfig::LIBXML2
    Threads::Threads
)

# 5. Main Application (GUI)
add_executable(EdavkiXmlMaker 
    src/main.cpp 
    include/gui/main_window.hpp
    include/gui/worker.hpp
    src/gui/main_window.cpp
    src/gui/worker.cpp
    resources/gui/resources.qrc
    ${CORE_OBJECTS}
)

target_include_directories(EdavkiXmlMaker PRIVATE ${EDAVKI_INCLUDES})
target_link_libraries(EdavkiXmlMaker PRIVATE 
    CoreLib 
    Qt6::Core 
    Qt6::Gui 
    Qt6::Widgets
)

# 6. Testing Setup
enable_testing()
include(GoogleTest)

# Non gui tests
function(add_edavki_test TEST_NAME SOURCE_FILE)
    add_executable(${TEST_NAME} ${SOURCE_FILE} tests/helper.cpp ${CORE_OBJECTS})
    
    target_include_directories(${TEST_NAME} PRIVATE ${EDAVKI_INCLUDES})
    
    target_compile_definitions(${TEST_NAME} PRIVATE 
        PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )

    target_link_libraries(${TEST_NAME} PRIVATE 
        CoreLib 
        GTest::gtest 
        GTest::gtest_main
    )
    
    gtest_discover_tests(${TEST_NAME})
endfunction()

add_edavki_test(test_report_loader tests/test_report_loader.cpp)
add_edavki_test(test_xml_generator tests/test_xml_generator.cpp)
add_edavki_test(test_application_service tests/test_application_service.cpp)

# GUI TEST (special tests)
add_executable(test_gui 
    tests/test_gui.cpp 
    include/gui/main_window.hpp
    include/gui/worker.hpp
    src/gui/main_window.cpp 
    src/gui/worker.cpp
    resources/gui/resources.qrc
    ${CORE_OBJECTS}
)

    target_include_directories(test_gui PRIVATE ${EDAVKI_INCLUDES})
target_link_libraries(test_gui PRIVATE 
    CoreLib 
    GTest::gtest 
    GTest::gtest_main 
    Qt6::Core 
    Qt6::Gui 
    Qt6::Widgets 
)
gtest_discover_tests(test_gui)