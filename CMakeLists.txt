cmake_minimum_required(VERSION 3.24)
project(TaxBrokerReport VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /WX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
else() # GCC
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Option to build legacy QT GUI
option(BUILD_LEGACY_GUI "Build the legacy Qt-based GUI codebase" OFF)

if(BUILD_LEGACY_GUI)
    add_subdirectory(legacy-QT-GUI)
endif()

include(FetchContent)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
)

FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
    csv_parser
    GIT_REPOSITORY https://github.com/vincentlaucsb/csv-parser.git
    GIT_TAG 2.1.3
)

FetchContent_GetProperties(csv_parser)
if(NOT csv_parser_POPULATED)
    FetchContent_Populate(csv_parser)
endif()

set(CSV_PARSER_INCLUDE_DIR "${csv_parser_SOURCE_DIR}/single_include")

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
