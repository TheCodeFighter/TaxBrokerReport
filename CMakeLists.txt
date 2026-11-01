cmake_minimum_required(VERSION 3.24)
project(TaxBrokerReport VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /WX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers)
else() # GCC
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers)
endif()

# Option to build legacy QT GUI
option(BUILD_LEGACY_GUI "Build the legacy Qt-based GUI codebase" OFF)
option(BUILD_DEBUG_TOOLS "Build local debugging tools" OFF)
# The libFuzzer targets (tests/fuzz) with AddressSanitizer and UndefinedBehaviorSanitizer on every
# project target. Clang only. This build has no unit or integration tests; use scripts/fuzz.sh.
option(TAXBROKER_BUILD_FUZZERS "Build the libFuzzer targets with sanitizers (Clang only)" OFF)

if(TAXBROKER_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "TAXBROKER_BUILD_FUZZERS needs Clang (libFuzzer).")
    endif()

    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(BUILD_LEGACY_GUI)
    add_subdirectory(legacy-QT-GUI)
endif()

include(FetchContent)

# The static release executables contain these dependencies: when one is added or its version
# changes, update THIRD_PARTY_NOTICES.txt.
find_package(Boost CONFIG REQUIRED)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
)

FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
    csv_parser
    URL https://github.com/vincentlaucsb/csv-parser/releases/download/2.5.2/csv.hpp
    URL_HASH SHA256=d28b5d474e4d46c0b04348f6b1ba11dd9212de10dd2231161888fd9d38254ed1
    DOWNLOAD_NO_EXTRACT TRUE
    SOURCE_SUBDIR no-cmake-project
)

FetchContent_MakeAvailable(csv_parser)

set(CSV_PARSER_INCLUDE_DIR "${csv_parser_SOURCE_DIR}")

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    pugixml
    URL https://github.com/zeux/pugixml/releases/download/v1.16/pugixml-1.16.tar.gz
    URL_HASH SHA256=4cee1ca4aad395170f4c7a07824f3bdd41f28316c6e1e1090a1425b278ec0b4b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(pugixml)

enable_testing()

add_subdirectory(src)
if(BUILD_DEBUG_TOOLS)
    add_subdirectory(tools)
endif()
if(TAXBROKER_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
else()
    add_subdirectory(tests)
endif()
