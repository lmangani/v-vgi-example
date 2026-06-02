# v-vgi-example

Query.Farm **VGI worker** example in [V](https://vlang.io/) — a drop-in subprocess worker for the `easter` catalog (scalar `easter_date`). The database host is **[Haybarn](https://github.com/Query-farm-haybarn/install)** (Query.Farm’s DuckDB distribution); the worker is V.

Golden IPC wire bytes are vendored under `vgi_v/testdata/`. Tests use V, C++ smoke clients, and optional Haybarn SQL.

## Goal

```sql
-- Haybarn ships as the CLI; load the VGI extension, then attach the worker:
LOAD '/path/to/vgi.duckdb_extension';
ATTACH 'easter' AS easter (TYPE vgi, LOCATION '/path/to/easter_worker');
SELECT easter.main.easter_date(2025);  -- 2025-04-20
```

## Prerequisites

| Tool | Notes |
|------|--------|
| [V](https://vlang.io/) | 0.5.x |
| Apache Arrow C++ | IPC shim + smoke clients; needs `pkg-config --libs arrow` |
| C++20 compiler | Builds `libvgi_ipc` (`.so` on Linux, `.dylib` on macOS) and smoke clients |
| [Haybarn](https://github.com/Query-farm-haybarn/install) | For full SQL e2e (`make test-haybarn`) |

**Apache Arrow** (examples):

```bash
# macOS
brew install apache-arrow pkg-config

# Debian/Ubuntu (Apache Arrow apt repo — needed on stock Ubuntu / GitHub runners)
sudo apt install -y ca-certificates lsb-release wget pkg-config g++
arch="$(lsb_release --id --short | tr 'A-Z' 'a-z')"
codename="$(lsb_release --codename --short)"
wget "https://packages.apache.org/artifactory/arrow/${arch}/apache-arrow-apt-source-latest-${codename}.deb"
sudo apt install -y ./apache-arrow-apt-source-latest-*.deb
sudo apt update
sudo apt install -y libarrow-dev

# Fedora
sudo dnf install -y gcc-c++ pkg-config arrow-devel
```

If `pkg-config` cannot find Arrow, set `PKG_CONFIG_PATH` to the directory containing `arrow.pc`.

### Install Haybarn

```bash
curl -fsSL https://query-farm-haybarn.github.io/install | sh
export PATH="$HOME/.haybarn/cli/latest:$PATH"
haybarn --version
```

See the [Haybarn installer README](https://github.com/Query-farm-haybarn/install) for pinned versions (`HAYBARN_VERSION=...`) and platforms (Linux/macOS, x86_64/arm64).

VGI is loaded as a **community extension** matching your Haybarn engine version (e.g. `v1.5.3`). `make test-haybarn` downloads it from `haybarn-community-extensions.query.farm` or uses `HAYBARN_VGI_EXTENSION` if set.

## Layout

```
vgi_v/
  ipc.v, worker.v, computus.v, easter_worker.v
  bind_*.v, catalog_*.v, init_*.v
  c/vgi_ipc.{cpp,h}, vgi_smoke_client.cpp, vgi_error_smoke_client.cpp
  testdata/*.bin
  computus_test.v
bin/vgi_smoke_client, vgi_error_smoke_client   # built by make
easter_worker                                   # built by make
scripts/
  gen_wire.sh, ensure_haybarn.sh, test_haybarn.sh, build_worker.sh
Makefile
```

## Build

```bash
make worker
# → ./easter_worker
```

## CI

GitHub Actions on `ubuntu-latest`: Apache [Arrow apt repo](https://arrow.apache.org/install/), V ([setup-v](https://github.com/vlang/setup-v)), Haybarn, then `make worker` and `make test-all`. See [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

## Test

### Fast (no Haybarn)

```bash
make test
```

| Target | What it checks |
|--------|----------------|
| `make test-computus` | Easter algorithm + date32 constants (V) |
| `make test-ipc` | stdio RPC happy path (`bind` → `init` → exchange) + EXCEPTION error batches |

### Full SQL e2e (Haybarn + VGI extension)

```bash
make test-haybarn
# or: make test-all   # unit + ipc + haybarn
```

Expect:

```
OK haybarn: easter_date [2024-03-31, 2025-04-20, 2026-04-05]
```

Override paths:

```bash
export HAYBARN="$HOME/.haybarn/cli/latest/haybarn"
export HAYBARN_VGI_EXTENSION=/path/to/vgi.duckdb_extension   # must match haybarn engine version (see haybarn --version)
export EASTER_WORKER="$(pwd)/easter_worker"
make test-haybarn
```

`test_haybarn.sh` resolves the extension in order: `HAYBARN_VGI_EXTENSION` → cache → [community download](https://haybarn-community-extensions.query.farm) → `INSTALL vgi FROM community` → local `~/.duckdb/extensions` and npm `@haybarn/ext-vgi-*` trees (probing `LOAD` for a match).

**Version lock:** extension engine version must match Haybarn’s DuckDB version (e.g. Haybarn `1.5.3-rc5` → `v1.5.3` extension). Pin Haybarn with `HAYBARN_VERSION` from [Haybarn releases](https://github.com/Query-farm-haybarn/haybarn/releases) if needed.

If Haybarn is missing, install via [the installer](https://github.com/Query-farm-haybarn/install) or set `HAYBARN=...`.

### Manual Haybarn session

Run `make test-haybarn` once to populate `.haybarn-cache/vgi-<engine>-<platform>.duckdb_extension`, or set `HAYBARN_VGI_EXTENSION` explicitly.

```bash
HAYBARN="$HOME/.haybarn/cli/latest/haybarn"
EXT="${HAYBARN_VGI_EXTENSION:-$(ls .haybarn-cache/vgi-*.duckdb_extension 2>/dev/null | head -1)}"
WORKER="$(pwd)/easter_worker"

"$HAYBARN" -unsigned -c "
  LOAD '${EXT}';
  ATTACH 'easter' AS easter (TYPE vgi, LOCATION '${WORKER}');
  SELECT easter.main.easter_date(2025);
"
```

Use `-unsigned` when loading a downloaded extension file ([Haybarn installer docs](https://github.com/Query-farm-haybarn/install)).

## RPC errors

Dispatch failures (unknown method, bad attach, etc.) return a **vgi-rpc EXCEPTION** batch on stdout: zero-row stream with `vgi_rpc.log_level=EXCEPTION`, message, and JSON `vgi_rpc.log_extra` (see [vgi-rpc wire protocol](https://vgi-rpc.query.farm/wire-protocol)). Unknown methods set `vgi_rpc.error_kind=method_not_implemented`.

## Regenerating wire constants

```bash
./scripts/gen_wire.sh   # reads vgi_v/testdata/*.bin → vgi_v/*.v
```

Add or replace `.bin` files under `vgi_v/testdata/`, then run `gen_wire.sh` again.

Haybarn 1.5.3 expects `late_materialization` on `catalog_schema_contents_functions` (31 catalog fields). If you recapture an older 30-field wire, run:

```bash
python3 scripts/patch_functions_wire_late_materialization.py
./scripts/gen_wire.sh
```

`make test-haybarn` prefers builtin VGI or `INSTALL vgi FROM community`; stale npm extension caches can mismatch the catalog schema.

## Status

Feature-complete for the Haybarn `easter` demo:

- [x] Easter computus in V
- [x] stdio RPC: `bind`, `init` / `exchange`, date32 result stream
- [x] Catalog RPCs: `catalog_catalogs`, `catalog_attach`, `catalog_schemas`, `catalog_schema_contents_schemas`, `catalog_schema_contents_functions`
- [x] `make test` — computus + IPC smoke (happy path + errors)
- [x] `make test-haybarn` — Haybarn SQL e2e (`make test-all`)
- [x] RPC EXCEPTION batches on dispatch failure

## References

- [Haybarn install](https://github.com/Query-farm-haybarn/install) — CLI installer
- [Haybarn releases](https://github.com/Query-farm-haybarn/haybarn/releases) — versioned binaries
- [vgi](https://github.com/Query-farm/vgi) — VGI DuckDB extension (prebuilds)
- [vgi-rpc wire protocol](https://vgi-rpc.query.farm/wire-protocol)
- [vgi-easter](https://github.com/Query-farm/vgi-easter) — reference catalog semantics
