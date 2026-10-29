#!/usr/bin/env python3
"""Forward a g++ command to tools/wine-mingw/g++.sh with the standalone -static token removed."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WRAPPER = os.path.normpath(os.path.join(HERE, "..", "..", "tools", "wine-mingw", "g++.sh"))

arguments = [argument for argument in sys.argv[1:] if argument != "-static"]
raise SystemExit(subprocess.call(["sh", WRAPPER] + arguments))
