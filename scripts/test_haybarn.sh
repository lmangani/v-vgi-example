#!/usr/bin/env bash
# End-to-end VGI test via Haybarn (DuckDB distro with community VGI extension).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORKER="${EASTER_WORKER:-$ROOT/easter_worker}"
HAYBARN="$("${ROOT}/scripts/ensure_haybarn.sh")"
CACHE_DIR="${HAYBARN_CACHE:-$ROOT/.haybarn-cache}"

if [[ ! -x "$WORKER" ]]; then
	echo "Building easter_worker ..." >&2
	make -C "$ROOT" worker
fi

engine_ver="$("$HAYBARN" --version 2>&1 | sed -n 's/.*DuckDB \(v[0-9.]*\).*/\1/p' | head -1)"
if [[ -z "$engine_ver" ]]; then
	engine_ver="v1.5.2"
fi
engine_tag="${engine_ver#v}"

os="$(uname -s)"
arch="$(uname -m)"
case "$os-$arch" in
	Darwin-arm64) platform=osx_arm64 ;;
	Darwin-x86_64) platform=osx_amd64 ;;
	Linux-x86_64) platform=linux_amd64 ;;
	Linux-aarch64 | Linux-arm64) platform=linux_arm64 ;;
	*)
		echo "Unsupported platform: $os $arch" >&2
		exit 1
		;;
esac

try_load_extension() {
	local ext="$1"
	"$HAYBARN" -unsigned -c "LOAD '${ext}';" >/dev/null 2>&1
}

try_load_vgi_builtin() {
	"$HAYBARN" -unsigned -c "LOAD vgi;" >/dev/null 2>&1
}

resolve_vgi_extension() {
	if [[ -z "${HAYBARN_VGI_EXTENSION:-}" ]] && try_load_vgi_builtin; then
		echo "builtin"
		return 0
	fi

	if [[ -n "${HAYBARN_VGI_EXTENSION:-}" && -f "$HAYBARN_VGI_EXTENSION" ]]; then
		if try_load_extension "$HAYBARN_VGI_EXTENSION"; then
			echo "$HAYBARN_VGI_EXTENSION"
			return 0
		fi
		echo "HAYBARN_VGI_EXTENSION does not load in this Haybarn (${engine_ver}): $HAYBARN_VGI_EXTENSION" >&2
		return 1
	fi

	local cached="$CACHE_DIR/vgi-${engine_tag}-${platform}.duckdb_extension"
	if [[ -f "$cached" ]] && try_load_extension "$cached"; then
		echo "$cached"
		return 0
	fi

	mkdir -p "$CACHE_DIR"
	local url="https://haybarn-community-extensions.query.farm/${engine_ver}/${platform}/vgi.duckdb_extension"
	echo "Downloading VGI extension: $url" >&2
	if curl -fsSL -o "$cached" "$url" 2>/dev/null && [[ -s "$cached" ]] && try_load_extension "$cached"; then
		echo "$cached"
		return 0
	fi
	rm -f "$cached"

	echo "Trying: INSTALL vgi FROM community;" >&2
	if "$HAYBARN" -unsigned -c "INSTALL vgi FROM community;" 2>/dev/null; then
		local installed="${HOME}/.duckdb/extensions/${engine_ver}/${platform}/vgi.duckdb_extension"
		if [[ -f "$installed" ]] && try_load_extension "$installed"; then
			cp "$installed" "$cached"
			echo "$cached"
			return 0
		fi
	fi

	echo "Searching local Haybarn / npm extension caches ..." >&2
	local candidate
	while IFS= read -r candidate; do
		if try_load_extension "$candidate"; then
			echo "$candidate"
			return 0
		fi
	done < <(
		find "${HOME}/.duckdb/extensions" "${HOME}/.npm" "${ROOT}/.haybarn-cache" -name 'vgi.duckdb_extension' 2>/dev/null \
			| sort -u
	)

	return 1
}

VGI_EXT="$(resolve_vgi_extension || true)"
if [[ -z "$VGI_EXT" ]]; then
	echo "Could not find VGI extension for Haybarn ${engine_ver} (${platform})." >&2
	echo "Set HAYBARN_VGI_EXTENSION=/path/to/vgi.duckdb_extension (must match engine version)." >&2
	exit 1
fi

WORKER_ABS="$(cd "$(dirname "$WORKER")" && pwd)/$(basename "$WORKER")"

if [[ -n "${VGI_DEBUG:-}" || -n "${VGI_DEBUG_LOG:-}" ]]; then
	: "${VGI_DEBUG:=1}"
	: "${VGI_DEBUG_LOG:=${TMPDIR:-/tmp}/vgi_rpc.log}"
	rm -f "$VGI_DEBUG_LOG"
	export VGI_DEBUG VGI_DEBUG_LOG
	echo "VGI_DEBUG_LOG=$VGI_DEBUG_LOG" >&2
fi
SQL="$(mktemp -t easter_haybarn.XXXXXX.sql)"
trap 'rm -f "$SQL"' EXIT
if [[ "$VGI_EXT" == "builtin" ]]; then
	load_sql="LOAD vgi;"
else
	load_sql="LOAD '${VGI_EXT}';"
fi

cat >"$SQL" <<EOF
${load_sql}
SELECT catalog FROM vgi_catalogs('${WORKER_ABS}');
ATTACH 'easter' AS easter (TYPE vgi, LOCATION '${WORKER_ABS}');
SELECT catalog_name, schema_name FROM information_schema.schemata WHERE catalog_name = 'easter';
SELECT easter.main.easter_date(2025) AS easter_2025;
SELECT easter.main.easter_date(y) AS easter_date
FROM (VALUES (2024), (2025), (2026)) AS t(y)
ORDER BY y;
EOF

echo "Haybarn: $HAYBARN ($("$HAYBARN" --version 2>&1 | head -1))"
echo "VGI extension: $VGI_EXT"
echo "Worker: $WORKER_ABS"

out="$("$HAYBARN" -unsigned -csv -f "$SQL" 2>&1)" || {
	echo "$out" >&2
	if [[ -n "${VGI_DEBUG_LOG:-}" && -f "$VGI_DEBUG_LOG" ]]; then
		echo "--- $VGI_DEBUG_LOG ---" >&2
		cat "$VGI_DEBUG_LOG" >&2
	fi
	exit 1
}

echo "$out"

fail=0
echo "$out" | grep -q 'easter' || { echo "missing catalog easter" >&2; fail=1; }
echo "$out" | grep -q 'main' || { echo "missing schema main" >&2; fail=1; }
echo "$out" | grep -q '2025-04-20' || { echo "missing 2025-04-20" >&2; fail=1; }
echo "$out" | grep -q '2024-03-31' || { echo "missing 2024-03-31" >&2; fail=1; }
echo "$out" | grep -q '2026-04-05' || { echo "missing 2026-04-05" >&2; fail=1; }

if [[ "$fail" -ne 0 ]]; then
	exit 1
fi

echo "OK haybarn: easter_date [2024-03-31, 2025-04-20, 2026-04-05]"
