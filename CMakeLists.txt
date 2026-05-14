cmake_minimum_required(VERSION 3.24)
project(TaxBrokerReport VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /WX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter)
else() # GCC
    add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter)
endif()

# Option to build legacy QT GUI
option(BUILD_LEGACY_GUI "Build the legacy Qt-based GUI codebase" OFF)

if(BUILD_LEGACY_GUI)
    add_subdirectory(legacy-QT-GUI)
endif()

include(FetchContent)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
