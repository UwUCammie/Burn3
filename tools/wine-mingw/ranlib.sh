#!/bin/sh
exec python3 "$(dirname "$0")/wine-tool.py" ranlib "$@"
