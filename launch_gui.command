#!/bin/bash
# macOS: double-click in Finder to open the launcher (launcher.py; see docs/MACOS.md).
cd -- "$(dirname -- "$0")"
PYTHON=$(command -v python3 || true)
if [[ -z $PYTHON ]] || ! "$PYTHON" -c 'import tkinter' 2>/dev/null; then
    echo 'Python 3.10+ with Tkinter is needed (python.org installer, or: brew install python-tk).'
    read -r -p 'Press Return to close.'
    exit 1
fi
exec "$PYTHON" launcher.py
