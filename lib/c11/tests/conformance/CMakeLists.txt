# SPDX-License-Identifier: Apache-2.0
# Generate and compile with the actual target toolchain (also during cross builds).
set(conformance_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${conformance_dir}")
set(conformance_outputs)
foreach(stem imported language metadata)
    list(APPEND conformance_outputs "${conformance_dir}/${stem}_types.c"
        "${conformance_dir}/${stem}_types.h")
endforeach()
list(APPEND conformance_outputs "${conformance_dir}/metadata_client.c" "${conformance_dir}/metadata_client.h"
    "${conformance_dir}/language_client.c" "${conformance_dir}/language_client.h")
add_custom_command(OUTPUT ${conformance_outputs}
    COMMAND "${c11_compiler}" -r --gen c11 -out "${conformance_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/language.thrift"
    COMMAND "${c11_compiler}" --gen c11 -out "${conformance_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/metadata.thrift"
    DEPENDS "${c11_compiler}" language.thrift imported.thrift metadata.thrift VERBATIM)
add_executable(thrift_c11_conformance test_conformance.c ${conformance_outputs})
target_include_directories(thrift_c11_conformance PRIVATE "${conformance_dir}")
target_link_libraries(thrift_c11_conformance PRIVATE thrift_c11)
set_target_properties(thrift_c11_conformance PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
thrift_c11_warnings(thrift_c11_conformance)
foreach(scenario constants defaults presence recursion services metadata)
    foreach(protocol binary compact)
        add_test(NAME thrift_c11_conformance_${scenario}_${protocol}
            COMMAND thrift_c11_conformance ${scenario} ${protocol})
        set_tests_properties(thrift_c11_conformance_${scenario}_${protocol}
            PROPERTIES LABELS "c11_conformance;semantic" TIMEOUT 30)
    endforeach()
endforeach()

# Reuse repository fixtures without copying or modifying their language constructs.
get_filename_component(thrift_source_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(corpus_names ThriftTest Recursive ManyTypedefs ManyOptionals EnumTest
    EnumContainersTest ConstantsDemo DoubleConstantsTest OptionalRequiredTest
    TypedefTest VoidMethExceptionsTest ExceptionStruct DebugProtoTest SmallTest)
foreach(stem IN LISTS corpus_names)
    set(idl "${thrift_source_root}/test/${stem}.thrift")
    if(NOT EXISTS "${idl}")
        message(FATAL_ERROR "C11 conformance corpus fixture missing: ${idl}")
    endif()
    thrift_c11_file_stem(file_stem "${stem}")
    set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/corpus/${stem}")
    file(MAKE_DIRECTORY "${output_dir}")
    set(corpus_outputs "${output_dir}/${file_stem}_types.c" "${output_dir}/${file_stem}_types.h")
    file(STRINGS "${idl}" service_declarations REGEX "^[ 	]*service[ 	]+")
    if(service_declarations)
        list(APPEND corpus_outputs "${output_dir}/${file_stem}_client.c" "${output_dir}/${file_stem}_client.h")
    endif()
    add_custom_command(OUTPUT ${corpus_outputs}
        COMMAND "${c11_compiler}" --gen c11 -out "${output_dir}" "${idl}"
        DEPENDS "${c11_compiler}" "${idl}" VERBATIM)
    # Include the generated header first; compile and link every generated object.
    file(WRITE "${output_dir}/main.c" "#include \"${file_stem}_types.h\"\nint main(void) { return 0; }\n")
    set(target "thrift_c11_corpus_${stem}")
    add_executable(${target} "${output_dir}/main.c" ${corpus_outputs})
    target_link_libraries(${target} PRIVATE thrift_c11)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    thrift_c11_warnings(${target})
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES LABELS "c11_conformance;compile_link" TIMEOUT 30)
endforeach()
if(Python3_Interpreter_FOUND)
    add_test(NAME thrift_c11_conformance_rejections COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_rejections.py" "${c11_compiler}")
    set_tests_properties(thrift_c11_conformance_rejections PROPERTIES LABELS "c11_conformance;negative" TIMEOUT 60)
    add_test(NAME thrift_c11_conformance_known_limitations COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_rejections.py" "${c11_compiler}" --known-limitations)
    set_tests_properties(thrift_c11_conformance_known_limitations
        PROPERTIES LABELS "c11_conformance;known_limitation" TIMEOUT 30)
endif()
