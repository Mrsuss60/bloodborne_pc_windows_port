"""Repository paths for the Python tests (run: python3 -m unittest discover -s tests)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))

for candidate in ['bb-probe.exe', 'bbport.exe', 'bb-probe', 'bbport']:
    p = ROOT / 'out' / candidate
    if p.exists():
        EXE = p
        break
else:
    EXE = ROOT / 'out' / 'bbport.exe'
