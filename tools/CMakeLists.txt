add_executable(taxbroker_tr_dump
    tr_parser_dump.cpp
)

target_link_libraries(taxbroker_tr_dump PRIVATE
    taxbroker_core
    taxbroker_api
)
