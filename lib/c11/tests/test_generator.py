# SPDX-License-Identifier: Apache-2.0
"""Generator diagnostics must reject input before writing broken C sources."""
import pathlib
import subprocess
import sys
import tempfile

PROCESS_TIMEOUT_SECONDS = 10
EXECUTABLE_ARGUMENT = 1



def main():
    compiler = sys.argv[EXECUTABLE_ARGUMENT]
    cases = [
        ("struct Item {}\nstruct ItemCreate {}", "c11", "collision"),
        ("namespace c11 invalid\nenum Types { H = 1 }", "c11", "collision"),
        ("exception E {} service S { i32 call() throws (0: E error) }",
         "c11", "duplicate field id"),
        ("struct FooBar {}\nstruct foo_bar {}", "c11", "collision"),
        ("struct Item {}\nstruct ItemNew {}", "c11", "collision"),
        ("enum E { THRIFT_RESERVED_WIDTH = 1 }", "c11", "collision"),
        ("enum E { value = 1, VALUE = 2 }", "c11", "collision"),
        ("union Choice { 1: i32 value }\nstruct ChoiceCase {}", "c11", "collision"),
        ("struct Bad { 1: i8 value = 300 }", "c11", "range"),
        ("struct Loop { 1: optional Loop next = {} }", "c11", "not defined"),
        ("struct Bad { 32768: i32 value }", "c11", "field id outside int16 range"),
        ("struct Bad { -32769: i32 value }", "c11", "field id outside int16 range"),
        ("service Bad { void call(32768: i32 value) }", "c11", "field id outside int16 range"),
        ("exception Error {} service Bad { void call() throws (32768: Error error) }",
         "c11", "field id outside int16 range"),
        ("struct Empty {}", "c11:unknown", "option"),
    ]
    for source, generator, diagnostic in cases:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = root / "invalid.thrift"
            path.write_text(source, encoding="utf-8")
            result = subprocess.run([compiler, "--allow-neg-keys", "--gen", generator, "-out", str(root), str(path)],
                                    capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
            if not result.returncode or diagnostic not in (result.stdout + result.stderr).lower():
                raise RuntimeError(f"Missing {diagnostic} diagnostic: {result.stdout} {result.stderr}")
            if list(root.glob("*_types.*")):
                raise RuntimeError("Invalid schema produced partial output")

    for stem, schema in (("123", "struct Item {}"),
                         ("Valid", "namespace c11 _reserved\nstruct Item {}")):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / (stem + ".thrift")
            source.write_text(schema, encoding="utf-8")
            result = subprocess.run([compiler, "--gen", "c11", "-out", str(root), str(source)],
                                    capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
            if result.returncode == 0 or "letter" not in result.stdout + result.stderr:
                raise RuntimeError("Invalid public identifier prefix was accepted")
            if list(root.glob("*_types.*")):
                raise RuntimeError("Invalid prefix produced output")

    # Shared namespaces are valid only if all public symbols remain distinct.
    for collision in (False, True):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "Left.thrift").write_text(
                "namespace c11 shared\nstruct Item { 1: list<i32> values }", encoding="utf-8")
            right_type = "Item" if collision else "Other"
            (root / "Right.thrift").write_text(
                f"namespace c11 shared\nstruct {right_type} {{ 1: list<i32> values }}",
                encoding="utf-8")
            source = root / "Root.thrift"
            source.write_text('include "Left.thrift"\ninclude "Right.thrift"', encoding="utf-8")
            result = subprocess.run([compiler, "--gen", "c11", "-out", str(root), str(source)],
                                    capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
            if collision:
                if result.returncode == 0 or "collision" not in result.stdout + result.stderr:
                    raise RuntimeError("Cross-module symbol collision was accepted")
                if list(root.glob("*_types.*")):
                    raise RuntimeError("Cross-module collision produced output")
            elif result.returncode:
                raise RuntimeError(f"Distinct shared-namespace types rejected: {result.stdout} {result.stderr}")

    # Header spelling is part of the source API, even when aliases have equal ABI.
    # These same fixtures are compiled by the conformance CMake targets.
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        fixtures = pathlib.Path(__file__).resolve().parent / "conformance"
        for stem in ("metadata", "language"):
            subprocess.run([compiler, "-r", "--gen", "c11", "-out", str(root),
                            str(fixtures / (stem + ".thrift"))], check=True,
                           capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
        header = (root / "metadata_types.h").read_text(encoding="utf-8")
        # Documentation must attach to the actual public declaration/member.
        documentation = {
            "Numeric identifier.": "typedef int32_t metadata_number;",
            "Documented enumeration.": "enum metadata_annotated {",
            "First enumeration value.": "METADATA_ANNOTATED_VALUE =",
            "Optional numeric field.": "metadata_number f_value;",
            "Service documentation.": "struct metadata_annotated_service_handler {",
            "Input collection documentation.": "metadata_numbers f_values;",
            "Documented exception.": "struct metadata_documented_error {",
            "Error message documentation.": "struct thrift_bytes f_message;",
            "Documented union.": "struct metadata_documented_choice {",
            "Declared exception documentation.": "struct metadata_documented_error * f_error;",
        }
        for doc, declaration in documentation.items():
            start = header.find(" * " + doc + "\n")
            if start < 0 or not header[start:].split("*/", 1)[1].lstrip().startswith(declaration):
                raise RuntimeError(f"Documentation is not attached to {declaration}")
        for text in (" * @file\n", "Accepted grammar metadata;", "Größe.",
                     " * A second paragraph with **Markdown**.", " * Default numeric constant."):
            if text not in header:
                raise RuntimeError(f"Documentation was lost: {text}")
        # Both inherited callbacks and inherited client calls retain method docs.
        if header.count(" * Legacy operation documentation.") != 4:
            raise RuntimeError("Method documentation is missing on client/server bindings")
        for declaration in (
            "metadata_number f_number;",
            "metadata_numbers f_numbers;",
            "metadata_attribute_alias f_attribute;",
            "metadata_earlier_alias f_forward_chain;",
            "metadata_node_alias f_next;",
            "struct metadata_annotated_record * f_reference;",
            "metadata_earlier_alias f_success;",
            "metadata_numbers f_values;",
            "metadata_const_aliased_value_get(metadata_earlier_alias *value);",
        ):
            if declaration not in header:
                raise RuntimeError(f"Missing generated declaration: {declaration}")
        header = (root / "language_types.h").read_text(encoding="utf-8")
        if "conform_item_alias f_item;" not in header:
            raise RuntimeError("Alias of an included record was lost")
        boundary = root / "boundary.thrift"
        boundary.write_text("struct Limits { -32768: i32 low; 0: i32 zero; 32767: i32 high }",
                            encoding="utf-8")
        subprocess.run([compiler, "--allow-neg-keys", "--gen", "c11", "-out", str(root), str(boundary)],
                       check=True, capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
        source = (root / "boundary_types.c").read_text(encoding="utf-8")
        for field_id in (-32768, 0, 32767):
            if f"{{{field_id}, offsetof" not in source:
                raise RuntimeError(f"Valid field ID was not preserved: {field_id}")

    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        (root / "SharedTypes.thrift").write_text(
            "namespace c11 bom\nstruct Item { 1: i32 value }", encoding="utf-8")
        (root / "ApiClient.thrift").write_text(
            'include "SharedTypes.thrift"\nnamespace c11 bom\n'
            'service Manager { SharedTypes.Item fetch() }', encoding="utf-8")
        subprocess.run([compiler, "-r", "--gen", "c11", "-out", str(root), str(root / "ApiClient.thrift")],
                       check=True, capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
        header = (root / "api_client_types.h").read_text(encoding="utf-8")
        shared = (root / "shared_types_types.h").read_text(encoding="utf-8")
        for declaration in ('#include "shared_types_types.h"', "struct bom_manager_handler",
                            "struct bom_item * f_success;", "bom_manager_fetch_call("):
            if declaration not in header:
                raise RuntimeError(f"Missing namespace/include declaration: {declaration}")
        for generated, guard in ((header, "API_CLIENT_TYPES_H"), (shared, "SHARED_TYPES_TYPES_H")):
            if f"#ifndef {guard}\n#define {guard}\n" not in generated:
                raise RuntimeError("Header guards must be uppercase and distinct per file")
        if sorted(path.name for path in root.glob("*_types.*")) != [
                "api_client_types.c", "api_client_types.h", "shared_types_types.c", "shared_types_types.h"]:
            raise RuntimeError("Generated filenames are not normalized")
        (root / "NoNamespace.thrift").write_text(
            "namespace cpp ignored\nstruct Item {}", encoding="utf-8")
        subprocess.run([compiler, "--gen", "c11", "-out", str(root), str(root / "NoNamespace.thrift")],
                       check=True, capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
        fallback_header = (root / "no_namespace_types.h").read_text(encoding="utf-8")
        if "struct no_namespace_item" not in fallback_header:
            raise RuntimeError("Missing filename-based fallback namespace")
        if " * @file\n" not in fallback_header:
            raise RuntimeError("Missing Doxygen file marker without IDL documentation")

    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        for name in ("FooBar", "foo_bar"):
            (root / (name + ".thrift")).write_text("struct Item {}", encoding="utf-8")
        source = root / "Collision.thrift"
        source.write_text('include "FooBar.thrift"\ninclude "foo_bar.thrift"', encoding="utf-8")
        result = subprocess.run([compiler, "--gen", "c11", "-out", str(root), str(source)],
                                capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
        if result.returncode == 0 or "filename collision" not in result.stdout + result.stderr:
            raise RuntimeError("Normalized filename collision was not rejected")
        if list(root.glob("*_types.*")):
            raise RuntimeError("Filename collision produced partial output")

    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        source = root / "ServerApi.thrift"
        source.write_text("namespace c11 app\nservice Base { void ping() }\n"
                          "service Child extends Base { i32 value() }\nservice Empty {}", encoding="utf-8")
        command = [compiler, "--gen", "c11:server_stubs", "-out", str(root), str(source)]
        subprocess.run(command, check=True, capture_output=True, timeout=PROCESS_TIMEOUT_SECONDS)
        header = (root / "server_api_server.h").read_text(encoding="utf-8")
        stub = root / "server_api_server.c"
        implementation = stub.read_text(encoding="utf-8")
        if "#ifndef SERVER_API_SERVER_H\n#define SERVER_API_SERVER_H" not in header:
            raise RuntimeError("Server stub guard must be uppercase")
        for declaration in ("app_child_server_init(", "app_child_server_process(", "app_empty_server_init("):
            if declaration not in header or declaration not in implementation:
                raise RuntimeError(f"Missing server entry point: {declaration}")
        for declaration in ("void *user_context", "void *handler_context",
                            "handler->user_context = user_context;",
                            "app_child_process(protocol, handler_context)"):
            if declaration not in implementation:
                raise RuntimeError(f"Ambiguous server context naming: {declaration}")
        types_header = (root / "server_api_types.h").read_text(encoding="utf-8")
        types_source = (root / "server_api_types.c").read_text(encoding="utf-8")
        if "void *user_context;" not in types_header or "handler->user_context, args, result" not in types_source:
            raise RuntimeError("Handler user data is not forwarded explicitly")
        if "handler->f_ping = app_child_server_handle_ping;" not in implementation:
            raise RuntimeError("Missing inherited server callback")
        client_header = (root / "server_api_client.h").read_text(encoding="utf-8")
        client_path = root / "server_api_client.c"
        client_source = client_path.read_text(encoding="utf-8")
        for declaration in ("app_child_client_init(", "app_child_client_ping_call(", "app_empty_client_init("):
            if declaration not in client_header or declaration not in client_source:
                raise RuntimeError(f"Missing fully generated client method: {declaration}")
        if "#ifndef SERVER_API_CLIENT_H\n#define SERVER_API_CLIENT_H" not in client_header:
            raise RuntimeError("Client header guard must be uppercase")
        client_path.write_text("/* Clients must regenerate completely. */", encoding="utf-8")
        edited = implementation + "\n/* Application implementation must survive regeneration. */\n"
        stub.write_text(edited, encoding="utf-8")
        subprocess.run(command, check=True, capture_output=True, timeout=PROCESS_TIMEOUT_SECONDS)
        if client_path.read_text(encoding="utf-8") != client_source:
            raise RuntimeError("Client implementation was not fully regenerated")
        if stub.read_text(encoding="utf-8") != edited:
            raise RuntimeError("Regeneration overwrote the application-owned server implementation")


if __name__ == "__main__":
    main()
