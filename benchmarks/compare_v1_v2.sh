#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BASE_TAG="${POLLNET_BASE_TAG:-v1.1.0}"
CXX_BIN="${CXX:-g++}"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

mkdir -p "$TMP_DIR/v1/include/pollnet"
if ! git -C "$REPO_ROOT" show "${BASE_TAG}:include/pollnet/socket.h" > "$TMP_DIR/v1/include/pollnet/socket.h"; then
    echo "Error: failed to extract ${BASE_TAG}:include/pollnet/socket.h" >&2
    exit 1
fi

COMMON_FLAGS=(-std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -pthread)

echo "[1/4] Compiling ${BASE_TAG} benchmark (${CXX_BIN})..."
"$CXX_BIN" "${COMMON_FLAGS[@]}" \
    -I"$TMP_DIR/v1/include" -I"$REPO_ROOT/benchmarks" \
    "$REPO_ROOT/benchmarks/bench_v1.cpp" -o "$TMP_DIR/bench_v1"

echo "[2/4] Compiling current v2.0.0 benchmark (${CXX_BIN})..."
"$CXX_BIN" "${COMMON_FLAGS[@]}" \
    -I"$REPO_ROOT/include" -I"$REPO_ROOT/benchmarks" \
    "$REPO_ROOT/benchmarks/bench_v2.cpp" -o "$TMP_DIR/bench_v2"

echo "[3/4] Running ${BASE_TAG} benchmark..."
"$TMP_DIR/bench_v1" --csv "$@" > "$TMP_DIR/v1.csv"

echo "[4/4] Running v2.0.0 benchmark..."
"$TMP_DIR/bench_v2" --csv "$@" > "$TMP_DIR/v2.csv"

echo
echo "=== pollnet Benchmark Comparison (${BASE_TAG} vs v2.0.0) ==="
echo "Args: ${*:-<defaults: warmup=2000, iters=50000, throughput_iters=200000, payload=64>}"
echo

awk -F'|' '
NR == FNR {
    if ($1 == "RESULT") {
        k = $2 "|" $3
        v1[k] = $4
        unit[k] = $5
    }
    next
}
{
    if ($1 == "RESULT") {
        k = $2 "|" $3
        keys[++n] = k
        bench[k] = $2
        metric[k] = $3
        v2[k] = $4
        unit[k] = $5
    }
}
END {
    printf "%-25s %-12s %12s %12s %11s  %-6s\n", "Benchmark", "Metric", "'"${BASE_TAG}"'", "v2.0.0", "Diff", "Unit"
    printf "%-25s %-12s %12s %12s %11s  %-6s\n", "-------------------------", "------------", "------------", "------------", "-----------", "------"
    prev_b = ""
    for (i = 1; i <= n; i++) {
        k = keys[i]
        b = bench[k]
        m = metric[k]
        if (m == "samples") continue
        if (prev_b != "" && prev_b != b) {
            printf "\n"
        }
        prev_b = b
        val2 = v2[k]
        u = unit[k]
        if (k in v1) {
            val1 = v1[k]
            if ((val1 + 0) > 0) {
                diff = ((val2 + 0) - (val1 + 0)) / (val1 + 0) * 100.0
                diff_str = sprintf("%+.2f%%", diff)
            } else {
                diff_str = "-"
            }
            printf "%-25s %-12s %12s %12s %11s  %-6s\n", b, m, val1, val2, diff_str, u
        } else {
            printf "%-25s %-12s %12s %12s %11s  %-6s\n", b, m, "-", val2, "-", u
        }
    }
}
' "$TMP_DIR/v1.csv" "$TMP_DIR/v2.csv"
