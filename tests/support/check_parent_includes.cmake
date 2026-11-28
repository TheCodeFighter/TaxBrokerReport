# Fails when a file in the CLI or server folder includes a header by a path that goes up a folder
# (#include "../processors/x.hpp", #include <../x.hpp>, ..\x.hpp). Such an include resolves next to
# the file, so it gets past the include-path check of the boundary probes.
# Usage: cmake -DROOT=<repository root> -DCLI_DIR=<src/cli> -DSERVER_DIR=<src/server> -P <this file>

set(CHECKED_DIRS "${CLI_DIR}" "${SERVER_DIR}")
set(VIOLATIONS "")
set(CHECKED_FILES 0)

foreach(DIR IN LISTS CHECKED_DIRS)
    file(GLOB_RECURSE SOURCE_FILES "${DIR}/*.cpp" "${DIR}/*.hpp")

    foreach(SOURCE_FILE IN LISTS SOURCE_FILES)
        math(EXPR CHECKED_FILES "${CHECKED_FILES} + 1")
        file(STRINGS "${SOURCE_FILE}" MATCHES
            REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"][^\">]*\\.\\.[/\\\\]")

        if(MATCHES)
            file(RELATIVE_PATH SHOWN_FILE "${ROOT}" "${SOURCE_FILE}")

            foreach(MATCH IN LISTS MATCHES)
                string(APPEND VIOLATIONS "${SHOWN_FILE}: ${MATCH}\n")
            endforeach()
        endif()
    endforeach()
endforeach()

if(CHECKED_FILES EQUAL 0)
    message(FATAL_ERROR "No source file was found to check: ${CHECKED_DIRS}")
endif()

if(VIOLATIONS)
    message(FATAL_ERROR "An include goes up a folder (resolved next to the file, past the "
                        "include-path check):\n${VIOLATIONS}")
endif()

message(STATUS "No include goes up a folder in ${CHECKED_FILES} files.")
