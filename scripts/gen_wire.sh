#!/usr/bin/env bash
# Generate vgi_v/*.v wire constants from vgi_v/testdata/*.bin (no code generation deps).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="$ROOT/vgi_v/testdata"
OUT_DIR="$ROOT/vgi_v"

shopt -s nullglob
bins=("$BIN_DIR"/*.bin)
if [[ ${#bins[@]} -eq 0 ]]; then
  echo "no .bin files in $BIN_DIR" >&2
  exit 1
fi

for path in "${bins[@]}"; do
  name="$(basename "$path" .bin)"
  out="$OUT_DIR/${name}.v"
  size=$(wc -c <"$path" | tr -d ' ')
  {
    echo "// ${name}.bin (${size} bytes)"
    echo "module vgi_v"
    echo ""
    echo "pub const ${name}_bin = ["
    while IFS= read -r hex; do
      line=""
      hex_len=${#hex}
      i=0
      while [ "$i" -lt "$hex_len" ]; do
        line+="u8(0x${hex:i:2}), "
        i=$((i + 2))
      done
      echo -e "\t${line%, },"
    done < <(xxd -p -c 16 "$path")
    echo "]"
  } >"$out"
  echo "wrote $(basename "$out") (${size} bytes)"
done
