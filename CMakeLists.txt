cmake_minimum_required(VERSION 3.30)

# -----------------------------
# Project settings
# -----------------------------
project(EdavkiXmlMaker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable all warnings and treat them as errors
if (MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Explicitly add debug flags for Debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
endif()

# -----------------------------
# Dependencies
# -----------------------------

# GTest
find_package(GTest REQUIRED)

# Poppler
find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(POPPLER QUIET poppler-cpp)
endif()

if (NOT POPPLER_FOUND)
    find_path(POPPLER_INCLUDE_DIR poppler/cpp/poppler-document.h)
    find_library(POPPLER_LIBRARY NAMES poppler-cpp)
    if (NOT POPPLER_INCLUDE_DIR OR NOT POPPLER_LIBRARY)
        message(FATAL_ERROR "Poppler not found. Please install Poppler or specify its location.")
    endif()
    add_library(Poppler::Poppler STATIC IMPORTED)
    set_target_properties(Poppler::Poppler PROPERTIES
        IMPORTED_LOCATION "${POPPLER_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${POPPLER_INCLUDE_DIR}"
    )
else()
    add_library(Poppler::Poppler INTERFACE IMPORTED)
    set_target_properties(Poppler::Poppler PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${POPPLER_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${POPPLER_LIBRARIES}"
    )
endif()

# pugixml
find_package(PkgConfig REQUIRED)
pkg_check_modules(PUGIXML REQUIRED pugixml)

# nlohmann-json
find_package(nlohmann_json REQUIRED)

# -----------------------------
# Main application target
# -----------------------------
add_executable(EdavkiXmlMaker
    src/main.cpp
    src/report_loader.cpp
    src/xml_generator.cpp
    src/platform_config.cpp
)

target_include_directories(EdavkiXmlMaker PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PUGIXML_INCLUDE_DIRS}
)

target_link_libraries(EdavkiXmlMaker PRIVATE
    Poppler::Poppler
    ${PUGIXML_LIBRARIES}
    nlohmann_json::nlohmann_json
    $<$<PLATFORM_ID:Linux>:pthread>
)

# -----------------------------
# Testing setup
# -----------------------------
enable_testing()

# -----------------------------
# Test 1: Report loader
# -----------------------------
add_executable(test_report_loader
    tests/test_report_loader.cpp
    src/report_loader.cpp
)

target_include_directories(test_report_loader PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PUGIXML_INCLUDE_DIRS}
)

target_link_libraries(test_report_loader PRIVATE
    GTest::gtest
    GTest::gtest_main
    Poppler::Poppler
    ${PUGIXML_LIBRARIES}
    nlohmann_json::nlohmann_json
    $<$<PLATFORM_ID:Linux>:pthread>
)

target_compile_definitions(test_report_loader PRIVATE
    UNIT_TEST
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME ReportLoaderTest COMMAND test_report_loader)

# -----------------------------
# Test 2: XML generator
# -----------------------------
add_executable(test_xml_generator
    tests/test_xml_generator.cpp
    src/xml_generator.cpp
)

target_include_directories(test_xml_generator PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PUGIXML_INCLUDE_DIRS}
)

target_link_libraries(test_xml_generator PRIVATE
    GTest::gtest
    GTest::gtest_main
    Poppler::Poppler
    ${PUGIXML_LIBRARIES}
    nlohmann_json::nlohmann_json
    $<$<PLATFORM_ID:Linux>:pthread>
)

target_compile_definitions(test_xml_generator PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME XmlGeneratorTest COMMAND test_xml_generator)

# -----------------------------
# Diagnostics
# -----------------------------
message(STATUS "C++ Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "C++ Compiler Version: ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "pugixml include dirs: ${PUGIXML_INCLUDE_DIRS}")
