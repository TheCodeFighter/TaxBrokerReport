# The libFuzzer target of the Trade Republic parser. It is built only with
# -DTAXBROKER_BUILD_FUZZERS=ON (Clang): the root CMakeLists.txt adds the sanitizers to every
# project target and skips the tests. Run it with scripts/fuzz.sh.
add_executable(traderepublic_parser_fuzz traderepublic_parser_fuzz.cpp)

target_include_directories(traderepublic_parser_fuzz PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CSV_PARSER_INCLUDE_DIR}
)

target_link_libraries(traderepublic_parser_fuzz PRIVATE taxbroker_core)

target_compile_options(traderepublic_parser_fuzz PRIVATE -fsanitize=fuzzer)
target_link_options(traderepublic_parser_fuzz PRIVATE -fsanitize=fuzzer)

# The multipart reader and the JSON readers of the local server take untrusted bytes from the
# browser. They are fuzzed next to the parser.
add_executable(multipart_fuzz multipart_fuzz.cpp)

target_include_directories(multipart_fuzz PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(multipart_fuzz PRIVATE taxbroker_server_lib)

target_compile_options(multipart_fuzz PRIVATE -fsanitize=fuzzer)
target_link_options(multipart_fuzz PRIVATE -fsanitize=fuzzer)

add_executable(answers_fuzz answers_fuzz.cpp)

target_include_directories(answers_fuzz PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(answers_fuzz PRIVATE taxbroker_api taxbroker_core)

target_compile_options(answers_fuzz PRIVATE -fsanitize=fuzzer)
target_link_options(answers_fuzz PRIVATE -fsanitize=fuzzer)
