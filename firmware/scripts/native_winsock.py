import sys

Import("env")  # noqa: F821  (injected by SCons)

if sys.platform == "win32":
    env.Append(LIBS=["ws2_32"])  # noqa: F821