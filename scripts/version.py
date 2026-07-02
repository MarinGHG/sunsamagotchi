Import("env")
import os
import subprocess

# FW_VERSION is set by CI to the exact release tag (build.yml derives it
# before building so both boards embed the same string). Local/dev builds
# fall back to `git describe`, then to a plain "dev-local" marker.
version = os.environ.get("FW_VERSION")
if not version:
    try:
        version = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=env["PROJECT_DIR"], stderr=subprocess.DEVNULL,
        ).decode().strip()
    except Exception:
        version = "dev-local"

env.Append(BUILD_FLAGS=[f'-DFIRMWARE_VERSION=\\"{version}\\"'])
