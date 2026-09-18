# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.18)
project(thrift_c11 VERSION 0.1.0 LANGUAGES C)
if(MSVC AND CMAKE_C_COMPILER_VERSION VERSION_LESS 19.28)
    message(FATAL_ERROR "The C11 runtime requires MSVC 19.28 or newer")
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
endif()
option(THRIFT_C11_ZSTD "Enable Zstandard transport" OFF)
option(THRIFT_C11_TLS "Enable nonblocking TLS sockets and framed server" OFF)
option(THRIFT_C11_HTTP_CLIENT "Enable libcurl HTTP client transport" OFF)
option(THRIFT_C11_HTTP_SERVER "Enable CivetWeb HTTP server" OFF)
option(THRIFT_C11_WEBSOCKET_CLIENT "Enable libcurl WebSocket client" OFF)
option(THRIFT_C11_WEBSOCKET_SERVER "Enable CivetWeb WebSocket server" OFF)
option(THRIFT_C11_EXAMPLES "Build generated C11 client/server examples" OFF)
option(THRIFT_C11_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(THRIFT_C11_WERROR "Treat C11 warnings as errors" OFF)
set(THRIFT_C11_COMPILER "" CACHE FILEPATH "Host Thrift compiler with the c11 generator")
set(THRIFT_C11_CPP_WEBSOCKET_PEER "" CACHE FILEPATH "Optional prebuilt C++ test_cpp_websocket_peer for wire interoperability tests")
set(THRIFT_C11_CADIN_ROOT "" CACHE PATH "Optional Cadin apisrv.parent checkout for original IDL regression tests and client")
add_library(thrift_c11 STATIC src/thrift/c11/thrift.c
    src/thrift/c11/thrift_log.c
    src/thrift/c11/protocol/thrift_protocol.c
    src/thrift/c11/protocol/thrift_binary_protocol.c
    src/thrift/c11/protocol/thrift_compact_protocol.c
    src/thrift/c11/protocol/thrift_multiplexed_protocol.c
    src/thrift/c11/transport/thrift_memory_buffer.c
    src/thrift/c11/processor/thrift_dispatch_processor.c
    src/thrift/c11/processor/thrift_multiplexed_processor.c
    src/thrift/c11/server/thrift_simple_server.c)
add_library(thrift::c11 ALIAS thrift_c11)
target_compile_features(thrift_c11 PUBLIC c_std_11)
set_target_properties(thrift_c11 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO EXPORT_NAME c11)
target_include_directories(thrift_c11 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(WIN32)
    target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_socket_windows.c
        src/thrift/c11/thrift_log_windows.c)
    target_link_libraries(thrift_c11 PRIVATE ws2_32)
else()
    target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_socket_posix.c
        src/thrift/c11/thrift_log_posix.c)
endif()
# Match the generator's identifier/file-stem normalization.
function(thrift_c11_file_stem output input)
    string(LENGTH "${input}" length)
    set(result "")
    set(previous "")
    set(index 0)
    while(index LESS length)
        string(SUBSTRING "${input}" ${index} 1 character)
        math(EXPR next_index "${index} + 1")
        set(next "")
        if(next_index LESS length)
            string(SUBSTRING "${input}" ${next_index} 1 next)
        endif()
        if(character MATCHES "[A-Z]" AND index GREATER 0 AND
           (previous MATCHES "[a-z]" OR next MATCHES "[a-z]"))
            string(APPEND result "_")
        endif()
        if(character MATCHES "[A-Za-z0-9]")
            string(TOLOWER "${character}" normalized)
            string(APPEND result "${normalized}")
        else()
            string(APPEND result "_")
        endif()
        set(previous "${character}")
        set(index ${next_index})
    endwhile()
    set(${output} "${result}" PARENT_SCOPE)
endfunction()

function(thrift_c11_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(THRIFT_C11_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(THRIFT_C11_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Registers <target>'s already-generated Thrift C11 sources (types, and client
# if present) and wires up its include directory, thrift_c11 and warnings.
# This does not invoke the compiler; generate the files first (see the c11
# README's `--gen c11[:server_stubs][,flat_calls]` commands) and regenerate
# manually after editing the IDL, same as temp_impl/generate.py. Available to
# in-tree consumers only (like thrift_c11_warnings): add_subdirectory this
# repository to use it; installed/find_package consumers wire sources by hand.
#
#   thrift_c11_add_generated(<target> BASE <base> [DIR <dir>] [SERVER_STUBS])
#
# BASE is the generated file stem, e.g. "calculator" for calculator_types.c.
# DIR defaults to CMAKE_CURRENT_SOURCE_DIR. SERVER_STUBS also requires and adds
# <base>_server.c.
#
# Every call checks the struct/enum names forward-declared in <base>_types.h
# against every other thrift_c11_add_generated() call so far in this configure
# run, and fails on an exact duplicate: two independently generated IDLs whose
# types happen to share both a C11 namespace and a type name would otherwise
# only surface as a link error. This checks actual declared type names, not
# just the C11 namespace string: two IDLs may share a namespace as long as
# their own type names differ (see the README's namespace section) - the
# earlier version of this check rejected that valid case. It intentionally
# does not know which targets end up linked together, so it also flags a
# genuine duplicate type name across targets that are never combined; treat a
# failure here as "investigate", not as certainly a real conflict, and the
# linker remains the authoritative check either way.
function(thrift_c11_add_generated target)
    set(options SERVER_STUBS)
    set(oneValueArgs BASE DIR)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "" ${ARGN})
    if(NOT ARG_BASE)
        message(FATAL_ERROR "thrift_c11_add_generated(${target}): BASE is required")
    endif()
    if(NOT ARG_DIR)
        set(ARG_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    set(header_path "${ARG_DIR}/${ARG_BASE}_types.h")
    if(EXISTS "${header_path}")
        file(READ "${header_path}" header_text)
        # CMake lists use ';' as the element separator; a raw ';' inside a
        # MATCHALL result (e.g. from a forward declaration's terminator) would
        # silently fragment that entry into two list items instead of one, so
        # placeholder it out of the text before matching.
        string(REPLACE ";" "@THRIFT_C11_SEMI@" header_text_safe "${header_text}")
        string(REGEX MATCHALL "struct [A-Za-z_][A-Za-z0-9_]*@THRIFT_C11_SEMI@" struct_decls "${header_text_safe}")
        string(REGEX MATCHALL "enum [A-Za-z_][A-Za-z0-9_]* \\{" enum_decls "${header_text_safe}")
        get_property(used GLOBAL PROPERTY THRIFT_C11_DECLARED_TYPES)
        foreach(declaration IN LISTS struct_decls enum_decls)
            string(REGEX REPLACE "^(struct|enum) ([A-Za-z_][A-Za-z0-9_]*).*$" "\\2" type_name "${declaration}")
            if(type_name IN_LIST used)
                message(FATAL_ERROR "thrift_c11_add_generated(${target}): '${type_name}' in ${header_path} "
                    "was already declared by another thrift_c11_add_generated() call in this configure. "
                    "Two independently generated IDLs are declaring the same C type name; give them "
                    "distinct IDL type names (a shared C11 namespace alone is fine).")
            endif()
            list(APPEND used "${type_name}")
        endforeach()
        set_property(GLOBAL PROPERTY THRIFT_C11_DECLARED_TYPES "${used}")
    endif()
    set(types_source "${ARG_DIR}/${ARG_BASE}_types.c")
    if(NOT EXISTS "${types_source}")
        message(FATAL_ERROR "thrift_c11_add_generated(${target}): ${types_source} does not exist; generate it first")
    endif()
    set(sources "${types_source}")
    set(client_source "${ARG_DIR}/${ARG_BASE}_client.c")
    if(EXISTS "${client_source}")
        list(APPEND sources "${client_source}")
    endif()
    if(ARG_SERVER_STUBS)
        set(server_source "${ARG_DIR}/${ARG_BASE}_server.c")
        if(NOT EXISTS "${server_source}")
            message(FATAL_ERROR "thrift_c11_add_generated(${target}): SERVER_STUBS requested "
                "but ${server_source} is missing; generate with --gen c11:server_stubs first")
        endif()
        list(APPEND sources "${server_source}")
    endif()
    target_sources(${target} PRIVATE ${sources})
    target_include_directories(${target} PRIVATE "${ARG_DIR}")
    target_link_libraries(${target} PRIVATE thrift_c11)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    thrift_c11_warnings(${target})
endfunction()
if(THRIFT_C11_TLS OR THRIFT_C11_ZSTD OR THRIFT_C11_HTTP_CLIENT OR THRIFT_C11_HTTP_SERVER OR
   THRIFT_C11_WEBSOCKET_CLIENT OR THRIFT_C11_WEBSOCKET_SERVER)
    target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_buffer.c)
endif()
if(THRIFT_C11_TLS)
    target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_tls_socket.c
        src/thrift/c11/transport/thrift_tls_client.c
        src/thrift/c11/server/thrift_tls_server.c)
    if(WIN32)
        target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_tls_net_windows.c
            src/thrift/c11/transport/thrift_tls_worker_windows.c
            src/thrift/c11/transport/thrift_tls_schannel_windows.c)
        target_link_libraries(thrift_c11 PRIVATE secur32 crypt32 ncrypt bcrypt)
    else()
        find_package(OpenSSL 1.1.1 REQUIRED)
        find_package(Threads REQUIRED)
        target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_tls_net_posix.c
            src/thrift/c11/transport/thrift_tls_worker_posix.c
            src/thrift/c11/transport/thrift_tls_openssl_posix.c)
        target_link_libraries(thrift_c11 PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
    endif()
endif()
if(THRIFT_C11_ZSTD)
    find_package(zstd 1.4 CONFIG REQUIRED)
    if(TARGET zstd::libzstd_static)
        set(c11_zstd_target zstd::libzstd_static)
    else()
        set(c11_zstd_target zstd::libzstd_shared)
    endif()
    target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_zstd_transport.c)
    target_link_libraries(thrift_c11 PRIVATE ${c11_zstd_target})
endif()
if(THRIFT_C11_HTTP_CLIENT OR THRIFT_C11_WEBSOCKET_CLIENT)
    if(THRIFT_C11_WEBSOCKET_CLIENT)
        find_package(CURL 8.14 REQUIRED)
    else()
        find_package(CURL 7.85 REQUIRED)
    endif()
    target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_curl.c)
    target_link_libraries(thrift_c11 PRIVATE CURL::libcurl)
endif()
if(THRIFT_C11_HTTP_CLIENT)
    target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_http_client.c)
endif()
if(THRIFT_C11_WEBSOCKET_CLIENT OR THRIFT_C11_WEBSOCKET_SERVER)
    target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_websocket.c)
endif()
if(THRIFT_C11_WEBSOCKET_CLIENT)
    target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_websocket_client.c)
    if(WIN32)
        target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_websocket_clock_windows.c)
    else()
        target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_websocket_clock_posix.c)
    endif()
endif()
if(THRIFT_C11_HTTP_SERVER OR THRIFT_C11_WEBSOCKET_SERVER)
    find_package(civetweb CONFIG REQUIRED)
    target_link_libraries(thrift_c11 PRIVATE civetweb::civetweb)
endif()
if(THRIFT_C11_WEBSOCKET_SERVER)
    include(CheckCSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_LIBRARIES civetweb::civetweb)
    unset(THRIFT_C11_CIVETWEB_LIMITS CACHE)
    check_c_source_compiles("#include <civetweb.h>
#if !defined(MG_WEBSOCKET_LIMITS_VERSION) || MG_WEBSOCKET_LIMITS_VERSION < 1
#error CivetWeb bounded WebSocket reader required
#endif
int main(void) { return mg_set_websocket_limits(0, 1, 1); }"
        THRIFT_C11_CIVETWEB_LIMITS)
    cmake_pop_check_state()
    if(NOT THRIFT_C11_CIVETWEB_LIMITS)
        message(FATAL_ERROR "WebSocket server requires the CivetWeb bounded-reader patch. See dependencies/civetweb/README.md; select its installation with civetweb_DIR.")
    endif()
    if(MSVC)
        if(CMAKE_C_COMPILER_VERSION VERSION_LESS 19.35)
            message(FATAL_ERROR "WebSocket server atomics require MSVC 19.35 or newer")
        endif()
        target_compile_options(thrift_c11 PRIVATE /experimental:c11atomics)
    endif()
    target_sources(thrift_c11 PRIVATE src/thrift/c11/server/thrift_websocket_server.c
        src/thrift/c11/transport/thrift_websocket_server_transport.c)
endif()
if(THRIFT_C11_HTTP_SERVER)
    target_sources(thrift_c11 PRIVATE src/thrift/c11/transport/thrift_http_server_transport.c
        src/thrift/c11/server/thrift_http_server.c)
endif()
thrift_c11_warnings(thrift_c11)
if(THRIFT_C11_SANITIZERS)
    if(MSVC OR WIN32)
        message(FATAL_ERROR "THRIFT_C11_SANITIZERS requires a Linux GCC/Clang toolchain")
    endif()
    target_compile_options(thrift_c11 PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(thrift_c11 PUBLIC -fsanitize=address,undefined)
endif()
install(TARGETS thrift_c11 EXPORT thrift_c11_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY src/thrift/c11 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/thrift
    FILES_MATCHING PATTERN "*.h" PATTERN "*_internal.h" EXCLUDE
    PATTERN "thrift_unix_socket.h" EXCLUDE)
if(NOT WIN32)
    install(FILES src/thrift/c11/transport/thrift_unix_socket.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/thrift/c11/transport)
endif()
install(EXPORT thrift_c11_targets NAMESPACE thrift:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/thrift_c11)
configure_package_config_file(cmake/thrift_c11Config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/thrift_c11Config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/thrift_c11)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/thrift_c11ConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/thrift_c11Config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/thrift_c11ConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/thrift_c11)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(THRIFT_C11_EXAMPLES)
    add_subdirectory(examples)
endif()
if(THRIFT_C11_CADIN_ROOT)
    add_subdirectory(examples/cadin)
endif()
