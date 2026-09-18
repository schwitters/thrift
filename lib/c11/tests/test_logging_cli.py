# SPDX-License-Identifier: Apache-2.0
"""Check log-level precedence and diagnostics without opening a network socket."""
import os
import subprocess
import sys

EXECUTABLE_ARGUMENT = 1
PROCESS_TIMEOUT_SECONDS = 10
LOG_LEVEL_ENVIRONMENT = "THRIFT_LOG_LEVEL"
SECRET_MARKER = "test-secret-must-not-appear"


def run(executable, environment_level=None, *options):
    environment = os.environ.copy()
    environment.pop(LOG_LEVEL_ENVIRONMENT, None)
    if environment_level is not None:
        environment[LOG_LEVEL_ENVIRONMENT] = environment_level
    # The socket API rejects destination port zero before DNS or connect.
    result = subprocess.run([executable, "--client", "0", *options], env=environment,
                            capture_output=True, text=True, timeout=PROCESS_TIMEOUT_SECONDS)
    assert result.returncode != 0
    assert not result.stdout
    return result.stderr


def main():
    executable = sys.argv[EXECUTABLE_ARGUMENT]
    default = run(executable)
    assert "level=info operation=socket.connect phase=begin" in default, default
    assert default.count("level=error") == 1, default
    error_only = run(executable, "error")
    assert "phase=begin" not in error_only, error_only
    assert error_only.count("level=error") == 1, error_only
    assert run(executable, "debug", "--log-level=fatal") == ""
    overridden = run(executable, SECRET_MARKER, "--log-level=info")
    assert "phase=begin" in overridden and SECRET_MARKER not in overridden, overridden
    invalid = run(executable, SECRET_MARKER)
    assert "Invalid log level" in invalid and "socket.connect" not in invalid, invalid
    assert SECRET_MARKER not in invalid
    invalid = run(executable, None, f"--log-level={SECRET_MARKER}")
    assert "Invalid log level" in invalid and SECRET_MARKER not in invalid, invalid
    duplicate = run(executable, None, "--log-level=info", "--log-level=debug")
    assert "Duplicate --log-level" in duplicate, duplicate


if __name__ == "__main__":
    main()
