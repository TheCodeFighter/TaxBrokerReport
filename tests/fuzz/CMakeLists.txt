# The libFuzzer target of the Trade Republic parser. It is built only with
# -DTAXBROKER_BUILD_FUZZERS=ON (Clang): the root CMakeLists.txt adds the sanitizers to every
# project target and skips the tests. Run it with scripts/fuzz.sh.
add_executable(traderepublic_parser_fuzz traderepublic_parser_fuzz.cpp)

target_include_directories(traderepublic_parser_fuzz PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CSV_PARSER_INCLUDE_DIR}
)

target_link_libraries(traderepublic_parser_fuzz PRIVATE taxbroker_core)

target_compile_options(traderepublic_parser_fuzz PRIVATE -fsanitize=fuzzer)
target_link_options(traderepublic_parser_fuzz PRIVATE -fsanitize=fuzzer)
