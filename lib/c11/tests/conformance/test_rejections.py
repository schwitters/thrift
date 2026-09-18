# SPDX-License-Identifier: Apache-2.0
"""Reject invalid schemas deliberately, with diagnostics and no partial C output."""
import argparse
import pathlib
import subprocess
import tempfile

PROCESS_TIMEOUT_SECONDS = 10
COMPILER_REJECTION_EXIT_CODE = 1

# Diagnostic fragments intentionally avoid paths, line numbers and full wording.
CASES = [
    ("syntax", "struct Bad { 1: i32 value", "syntax"),
    ("unknown_type", "struct Bad { 1: Unknown value }", "not defined"),
    ("duplicate_type", "struct Same {} struct Same {}", "already defined"),
    ("duplicate_field_id", "struct Bad { 1: i32 a; 1: i32 b }", "already been used"),
    ("duplicate_field_name", "struct Bad { 1: i32 a; 2: i32 a }", "already been used"),
    ("enum_overflow", "enum Bad { VALUE = 2147483648 }", "64-bit"),
    ("byte_underflow", "const byte BAD = -129", "range"),
    ("byte_overflow", "const i8 BAD = 128", "range"),
    ("short_underflow", "const i16 BAD = -32769", "range"),
    ("short_overflow", "const i16 BAD = 32768", "range"),
    ("int_underflow", "const i32 BAD = -2147483649", "range"),
    ("int_overflow", "const i32 BAD = 2147483648", "range"),
    ("boolean_range", "const bool BAD = 2", "range"),
    ("default_overflow", "struct Bad { 1: i16 value = 32768 }", "range"),
    ("nested_overflow", "const list<i8> BAD = [0, 128]", "range"),
    ("wrong_constant_type", 'const i32 BAD = "text"', "type error"),
    ("unknown_constant_field", 'struct Item { 1: i32 value } const Item BAD = {"missing": 1}', "no field"),
    ("invalid_uuid", 'const uuid BAD = "not-a-uuid"', "uuid"),
    ("unknown_base", "service Bad extends Missing {}", "not been defined"),
    ("invalid_throws", "service Bad { void fail() throws (1: i32 error) }", "non-exception"),
    ("oneway_return", "service Bad { oneway i32 fail() }", "oneway"),
    ("oneway_throws", "exception Error {} service Bad { oneway void fail() throws (1: Error error) }", "oneway"),
    ("union_defaults", "union Bad { 1: i32 a = 1; 2: i32 b = 2 }", "default"),
    ("type_collision", "struct FooBar {} struct foo_bar {}", "collision"),
    ("field_collision", "struct Bad { 1: i32 fooBar; 2: i32 foo_bar }", "collision"),
    ("method_collision", "service Bad { void fooBar(); void foo_bar() }", "collision"),
    ("inherited_collision", "service Base { void fooBar() } service Bad extends Base { void foo_bar() }", "collision"),
]

# These are accepted IDL intentions blocked by this checkout's shared frontend.
# Keep them out of invalid-input counts. An unexpected success requires reclassifying
# the case as supported and adding a positive compile/runtime regression.
KNOWN_LIMITATIONS = [
    ("struct_constant_default", 'struct Item { 1: i32 value } const Item VALUE = {"value": 7} '
     'struct Holder { 1: optional Item item = VALUE }', "type error"),
]


def run_cases(compiler, cases):
    failures = []
    for name, source, diagnostic in cases:
        with tempfile.TemporaryDirectory(prefix="c11-conformance-") as directory:
            root = pathlib.Path(directory)
            schema = root / "input.thrift"
            schema.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [compiler, "--gen", "c11", "-out", str(root), str(schema)],
                capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
            output = result.stdout + result.stderr
            if result.returncode != COMPILER_REJECTION_EXIT_CODE or diagnostic not in output.lower():
                failures.append(f"{name}: expected rejection containing {diagnostic!r}; "
                                f"exit={result.returncode}\n{output}")
            elif list(root.glob("*_types.*")):
                failures.append(f"{name}: rejected input left partial generated sources")
            else:
                print(f"PASS {name}")
    if failures:
        raise RuntimeError("\n".join(failures))
    print(f"Checked {len(cases)} cases")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler")
    parser.add_argument("--known-limitations", action="store_true")
    args = parser.parse_args()
    run_cases(args.compiler, KNOWN_LIMITATIONS if args.known_limitations else CASES)


if __name__ == "__main__":
    main()
