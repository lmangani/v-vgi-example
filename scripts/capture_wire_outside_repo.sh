#!/usr/bin/env bash
# Optional maintainer helper: capture golden IPC wire into vgi_v/testdata/*.bin using external Query.Farm worker checkouts (not used by make test).
# Not invoked by make test. Example:
#
#   export VGI_PYTHON=/path/to/vgi-python
#   export VGI_EASTER=/path/to/vgi-easter
#   ./scripts/capture_wire_outside_repo.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VGI_PYTHON="${VGI_PYTHON:?set VGI_PYTHON}"
VGI_EASTER="${VGI_EASTER:?set VGI_EASTER}"
OUT="$ROOT/vgi_v/testdata"

if [[ ! -f "$VGI_PYTHON/vgi/protocol.py" ]]; then
	echo "vgi-python not found at $VGI_PYTHON" >&2
	exit 1
fi

mkdir -p "$OUT"
cd "$VGI_PYTHON"
UV_PYTHON="${UV_PYTHON:-python3.13}" uv run python3 <<PY
import os, sys
from pathlib import Path
os.environ.setdefault("VGI_EASTER_GIT_COMMIT", "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
sys.path.insert(0, os.environ["VGI_EASTER"])
OUT = Path("${OUT}")
# Reuse capture logic: run scripts/capture_catalog_wire.py if present in repo clone
cap = Path("${ROOT}") / "scripts" / "capture_catalog_wire.py"
if cap.exists():
    exec(cap.read_text(), {"__name__": "__main__"})
else:
    raise SystemExit("Add capture_catalog_wire.py or run from a checkout that has it")
PY

cd "$ROOT"
./scripts/gen_wire.sh
echo "Captured wire -> $OUT and regenerated vgi_v/*.v"
