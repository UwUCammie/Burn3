#!/usr/bin/env python3
"""Run the bundled Windows MinGW tools under Wine from a Linux CMake build.

CMake on Linux passes POSIX paths. Wine's Z: drive maps /, so converting an
absolute POSIX path to Z:\\... lets the Windows compiler, assembler and linker
consume it without a per-argument winepath process.
"""
import os
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PREFIX = os.path.join(ROOT, "runtime", "setup", "compiler")
BIN = os.path.join(PREFIX, "bin")
WINEPREFIX = os.environ.get("WINEPREFIX", os.path.join(ROOT, "tmp", "burnout-wineprefix"))

TOOLS = {
    "g++": "bin/g++.exe",
    "gcc": "bin/gcc.exe",
    "ar": "bin/ar.exe",
    "ranlib": "bin/ranlib.exe",
    "windres": "x86_64-w64-mingw32/bin/windres.exe",
    "nm": "x86_64-w64-mingw32/bin/nm.exe",
}


def windows_path(value: str) -> str:
    if value.startswith("/"):
        return "Z:" + value.replace("/", "\\")
    return value


def convert_argument(argument: str) -> str:
    if argument.startswith("@"):
        path = argument[1:]
        if not os.path.isfile(path):
            return argument
        with open(path, "r", encoding="utf-8", errors="replace") as stream:
            entries = [line.rstrip("\n") for line in stream]
        converted = [convert_argument(entry.strip()) for entry in entries if entry.strip()]
        response = path + ".wine.rsp"
        with open(response, "w", encoding="utf-8") as stream:
            stream.write("\n".join(converted))
        return "@" + windows_path(response)
    for prefix in ("-I", "-L", "-isystem", "-include", "-imacros", "-MF", "-MT", "-MQ"):
        if argument.startswith(prefix) and len(argument) > len(prefix):
            value = argument[len(prefix):]
            if value.startswith("/"):
                return prefix + windows_path(value)
    if argument.startswith("--sysroot="):
        return "--sysroot=" + windows_path(argument[len("--sysroot="):])
    if argument.startswith("-Wl,"):
        parts = argument.split(",")
        return ",".join(windows_path(part) if part.startswith("/") else part for part in parts)
    if argument.startswith("/") and not argument.startswith("//"):
        return windows_path(argument)
    return argument


def posix_path(value: str) -> str:
    if value.startswith("Z:\\") or value.startswith("Z:/"):
        return "/" + value[3:].replace("\\", "/")
    return value


def clean_depfile(path: str) -> None:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as stream:
            lines = stream.read().splitlines()
    except OSError:
        return
    converted = []
    for line in lines:
        text = line.replace("Z:\\", "/").replace("Z:/", "/")
        result = []
        index = 0
        while index < len(text):
            char = text[index]
            if char == "\\" and (index + 1 == len(text) or text[index + 1] in " #"):
                result.append(char)
                index += 1
            elif char == "\\":
                result.append("/")
            else:
                result.append(char)
            index += 1
        converted.append("".join(result))
    with open(path, "w", encoding="utf-8") as stream:
        stream.write("\n".join(converted) + "\n")


def dependency_file(arguments: list[str]) -> str | None:
    for index, argument in enumerate(arguments):
        if argument == "-MF" and index + 1 < len(arguments):
            return posix_path(arguments[index + 1])
        if argument.startswith("-MF") and len(argument) > 3:
            return posix_path(argument[3:])
    return None


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in TOOLS:
        print("usage: wine-tool.py {g++|gcc|ar|ranlib} ARGS...", file=sys.stderr)
        return 2
    tool = TOOLS[sys.argv[1]]
    arguments = [convert_argument(argument) for argument in sys.argv[2:]]
    if sys.argv[1] in ("g++", "gcc") and not any(a.startswith("--sysroot=") for a in arguments):
        arguments.insert(0, "--sysroot=" + windows_path(PREFIX))
    if sys.argv[1] == "windres":
        arguments[0:0] = ["-I", windows_path(os.path.join(PREFIX, "include"))]
    environment = os.environ.copy()
    environment.setdefault("WINEDEBUG", "-all")
    environment["WINEPREFIX"] = WINEPREFIX
    environment["WINEPATH"] = windows_path(BIN) + ";" + windows_path(os.path.join(PREFIX, "x86_64-w64-mingw32", "bin"))
    command = ["wine", windows_path(os.path.join(PREFIX, tool)), *arguments]
    status = subprocess.call(command, env=environment)
    if status == 0:
        depfile = dependency_file(arguments)
        if depfile is not None:
            clean_depfile(depfile)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
