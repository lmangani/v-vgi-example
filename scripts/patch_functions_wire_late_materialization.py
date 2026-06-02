#!/usr/bin/env python3
"""Add late_materialization to catalog_catalog_schema_contents_functions golden wire.

Haybarn 1.5.3 (builtin VGI on Linux) expects 31 catalog fields; older captures had 30.
Run from repo root: python3 scripts/patch_functions_wire_late_materialization.py
Then: ./scripts/gen_wire.sh
"""
from __future__ import annotations

import sys
from pathlib import Path

import pyarrow as pa
from pyarrow import ipc

ROOT = Path(__file__).resolve().parents[1]
WIRE = ROOT / "vgi_v/testdata/catalog_catalog_schema_contents_functions_wire.bin"


def read_stream(data: bytes) -> tuple[pa.Schema, list[pa.RecordBatch]]:
    with ipc.open_stream(data) as reader:
        return reader.schema, list(reader)


def write_stream(schema: pa.Schema, batches: list[pa.RecordBatch]) -> bytes:
    sink = pa.BufferOutputStream()
    with ipc.new_stream(sink, schema) as writer:
        for batch in batches:
            writer.write_batch(batch)
    return sink.getvalue().to_pybytes()


def patch_catalog_blob(blob: bytes) -> bytes:
    cat_schema, cat_batches = read_stream(blob)
    names = [f.name for f in cat_schema]
    if "late_materialization" in names:
        return blob

    insert_at = names.index("sampling_pushdown") + 1
    if names[insert_at] != "supported_expression_filters":
        raise SystemExit(f"unexpected field at {insert_at}: {names[insert_at]}")

    new_fields = list(cat_schema)
    new_fields.insert(insert_at, pa.field("late_materialization", pa.bool_(), nullable=True))
    new_cat_schema = pa.schema(new_fields)

    patched_batches: list[pa.RecordBatch] = []
    for batch in cat_batches:
        arrays = [batch.column(i) for i in range(len(cat_schema))]
        arrays.insert(insert_at, pa.array([None] * batch.num_rows, type=pa.bool_()))
        patched_batches.append(pa.RecordBatch.from_arrays(arrays, schema=new_cat_schema))
    return write_stream(new_cat_schema, patched_batches)


def patch_functions_wire(data: bytes) -> bytes:
    outer_schema, outer_batches = read_stream(data)
    outer_batch = outer_batches[0]
    result_idx = outer_schema.get_field_index("result")
    result_bytes = outer_batch.column(result_idx)[0].as_py()

    items_schema, items_batches = read_stream(result_bytes)
    items_batch = items_batches[0]
    items_list = items_batch.column(0)[0].as_py()

    new_items = [patch_catalog_blob(blob) for blob in items_list]
    new_items_col = pa.array([new_items], type=pa.list_(pa.binary()))
    new_items_batch = pa.RecordBatch.from_arrays([new_items_col], schema=items_schema)
    new_result_bytes = write_stream(items_schema, [new_items_batch])

    new_outer_arrays = [outer_batch.column(i) for i in range(len(outer_schema))]
    new_outer_arrays[result_idx] = pa.array([new_result_bytes], type=pa.binary())
    new_outer_batch = pa.RecordBatch.from_arrays(new_outer_arrays, schema=outer_schema)
    return write_stream(outer_schema, [new_outer_batch])


def main() -> int:
    if not WIRE.is_file():
        print(f"missing {WIRE}", file=sys.stderr)
        return 1

    before = WIRE.read_bytes()
    after = patch_functions_wire(before)

    outer_schema, outer_batches = read_stream(after)
    result_bytes = outer_batches[0].column(outer_schema.get_field_index("result"))[0].as_py()
    _, items_batches = read_stream(result_bytes)
    blob = items_batches[0].column(0)[0].as_py()[0]
    cat_schema, _ = read_stream(blob)
    names = [f.name for f in cat_schema]
    if len(names) != 31 or "late_materialization" not in names:
        print("verification failed:", len(names), names, file=sys.stderr)
        return 1

    WIRE.write_bytes(after)
    print(f"patched {WIRE.name}: {len(before)} -> {len(after)} bytes, {len(names)} catalog fields")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
