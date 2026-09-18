# SPDX-License-Identifier: Apache-2.0
# calculator_types.*, calculator_client.* and calculator_server.* are checked-in
# generated output (like temp_impl/), not rebuilt from calculator.thrift here.
# calculator_server.c is application-owned: `--gen c11:server_stubs` preserves it
# across regeneration. flat_calls additionally emits example_calculator_client_add()
# and, since server_stubs is also on, a matching flat example_calculator_server_add()
# callback for calculator_server.c to implement (plain parameters/output pointers
# instead of args/result records) since add() has no optional argument and a
# scalar return. To regenerate after editing calculator.thrift, run:
#   <thrift-compiler> --gen c11:server_stubs,flat_calls -out lib/c11/examples/ lib/c11/examples/calculator.thrift
add_executable(thrift_c11_calculator calculator.c)
thrift_c11_add_generated(thrift_c11_calculator BASE calculator SERVER_STUBS)
if(BUILD_TESTING)
    add_test(NAME thrift_c11_calculator_help COMMAND thrift_c11_calculator --help)
    add_test(NAME thrift_c11_calculator_bad_port COMMAND thrift_c11_calculator --client 65536)
    set_tests_properties(thrift_c11_calculator_bad_port PROPERTIES WILL_FAIL TRUE)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME thrift_c11_tcp COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_tcp.py" "$<TARGET_FILE:thrift_c11_calculator>")
        set_tests_properties(thrift_c11_tcp PROPERTIES TIMEOUT 30)
    endif()
endif()
if(BUILD_TESTING AND Python3_Interpreter_FOUND AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME thrift_c11_logging_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_logging_cli.py" "$<TARGET_FILE:thrift_c11_calculator>")
    set_tests_properties(thrift_c11_logging_cli PROPERTIES TIMEOUT 30)
endif()
