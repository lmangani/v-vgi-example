#!/usr/bin/env bash
# Install or locate Haybarn CLI (Query.Farm DuckDB distro).
# https://github.com/Query-farm-haybarn/install
set -euo pipefail

HAYBARN="${HAYBARN:-}"

if [[ -n "$HAYBARN" && -x "$HAYBARN" ]]; then
	echo "$HAYBARN"
	exit 0
fi

for candidate in \
	"${HOME}/.haybarn/cli/latest/haybarn" \
	"${HOME}/.local/bin/haybarn" \
	"$(command -v haybarn 2>/dev/null || true)"; do
	if [[ -n "$candidate" && -x "$candidate" ]]; then
		echo "$candidate"
		exit 0
	fi
done

if [[ "${HAYBARN_INSTALL:-0}" == "1" ]]; then
	echo "Installing Haybarn via https://query-farm-haybarn.github.io/install ..." >&2
	curl -fsSL https://query-farm-haybarn.github.io/install | sh
	if [[ -x "${HOME}/.haybarn/cli/latest/haybarn" ]]; then
		echo "${HOME}/.haybarn/cli/latest/haybarn"
		exit 0
	fi
fi

echo "Haybarn not found. Install with:" >&2
echo "  curl -fsSL https://query-farm-haybarn.github.io/install | sh" >&2
echo "  export PATH=\"\${HOME}/.haybarn/cli/latest:\$PATH\"" >&2
echo "Or set HAYBARN=/path/to/haybarn and retry." >&2
exit 1
