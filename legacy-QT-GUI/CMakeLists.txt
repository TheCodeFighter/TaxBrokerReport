cmake_minimum_required(VERSION 3.20)
project(EdavkiXmlMaker VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Configuration & Dependencies

# Qt6 Setup
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(QT_LIBS Qt6::Core Qt6::Gui Qt6::Widgets)

# Performance
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message(STATUS "Using ccache")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

if(WIN32)
    set(THREADS_PREFER_PTHREAD_FLAG OFF)
endif()

# Build Strategy
string(TOUPPER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_UPPER)
if(CMAKE_BUILD_TYPE_UPPER STREQUAL "DEBUG")
    message(STATUS "Build Mode: DEBUG (Coverage Enabled)")
    add_compile_options(--coverage -fno-inline -fno-inline-small-functions -fno-default-inline)
    add_link_options(--coverage)
    add_compile_definitions(UNIT_TEST)
    set(CORE_LIB_TYPE OBJECT)
    set(CORE_OBJECTS $<TARGET_OBJECTS:CoreLib>)
else()
    set(CORE_LIB_TYPE STATIC)
    set(CORE_OBJECTS "")
endif()

# External Libraries
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

find_package(PkgConfig REQUIRED)

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    
    find_package(pugixml CONFIG REQUIRED)
    find_package(LibXml2 REQUIRED)
    
    add_library(PkgConfig::PUGIXML ALIAS pugixml::pugixml)
    add_library(PkgConfig::LIBXML2 ALIAS LibXml2::LibXml2)

    # Force CMake to find vcpkg's pkg-config files
    set(ENV{PKG_CONFIG_PATH} "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/lib/pkgconfig")
    
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(POPPLER REQUIRED IMPORTED_TARGET poppler-cpp)
else()
    pkg_check_modules(POPPLER REQUIRED IMPORTED_TARGET poppler-cpp)
    pkg_check_modules(PUGIXML REQUIRED IMPORTED_TARGET pugixml)
    pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)
endif()

# Global Includes
set(EDAVKI_INCLUDES 
    include 
    include/api 
    include/backend 
    include/gui
    include/util
)

# Building for release
if(STATIC_BUILD AND NOT WIN32)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a;.lib")
    add_definitions(-DQT_STATIC)
endif()

# 2. Core Library
add_library(CoreLib ${CORE_LIB_TYPE}
    src/backend/report_loader.cpp
    src/backend/xml_generator.cpp
    src/api/application_service.cpp
    src/util/util_xml.cpp
    src/util/config.cpp
)

target_include_directories(CoreLib PUBLIC ${EDAVKI_INCLUDES})
target_link_libraries(CoreLib PUBLIC 
    nlohmann_json::nlohmann_json
    PkgConfig::PUGIXML
    PkgConfig::POPPLER
    PkgConfig::LIBXML2
    Threads::Threads
)

# 3. Main Application
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
target_link_libraries(EdavkiXmlMaker PRIVATE CoreLib ${QT_LIBS})

# 4. Windows Deployment
if(WIN32)
    set_target_properties(EdavkiXmlMaker PROPERTIES WIN32_EXECUTABLE TRUE)
    
    # 1. Install the Main Exe
    install(TARGETS EdavkiXmlMaker RUNTIME DESTINATION .)
    
    # 2. Deploy Qt DLLs (Core, Gui, Widgets, Plugins)
    install(CODE "
        execute_process(COMMAND windeployqt --release --no-translations \"\${CMAKE_INSTALL_PREFIX}/EdavkiXmlMaker.exe\")
    ")
    
    # 3. Deploy VCPKG DLLs (Poppler, LibXml2, PugiXML, Zlib, etc.)
    # We simply copy ALL DLLs from the vcpkg bin folder to be safe.
    install(DIRECTORY "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/bin/"
            DESTINATION .
            FILES_MATCHING PATTERN "*.dll")
endif()

# 5. MacOS Deployment
if(APPLE)
    # This makes it a proper .app bundle instead of just a naked binary
    set_target_properties(EdavkiXmlMaker PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "EdavkiXmlMaker"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.taxbroker.edavkixml"
    )
    install(TARGETS EdavkiXmlMaker BUNDLE DESTINATION .)
endif()

# 6. Linux Deployment
if(UNIX AND NOT APPLE)
    install(TARGETS EdavkiXmlMaker RUNTIME DESTINATION bin)
endif()

# Tests
enable_testing()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    add_subdirectory(tests)
endif()