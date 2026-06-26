# PlatformIO pre-build hook: stamp the firmware version from git tags.
# Runs `git describe` matching the firmware tag line (fw-v*), strips the
# "fw-" prefix, and injects it as the FIRMWARE_VERSION C string macro.
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)


def firmware_version():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--match", "fw-v*", "--always", "--dirty"],
            stderr=subprocess.DEVNULL,
        )
        version = out.decode().strip()
    except Exception:
        return "unknown"
    if version.startswith("fw-"):
        version = version[len("fw-"):]
    return version or "unknown"


version = firmware_version()
print("firmware version: %s" % version)
env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(version))])
