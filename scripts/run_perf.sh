#!/usr/bin/env bash
# scripts/run_perf.sh — automated benchmark execution + perf-stat stage.
#
# Audit task 0.2 requires two things: automated execution of the native
# benchmark binaries (statistical latency output) and a Linux `perf stat`
# pass recording L1-dcache-load-misses, instructions and cycles.
#
# Usage:
#   scripts/run_perf.sh [--json DIR] [--no-perf] [BINARY|DIRECTORY ...]
#
#   With no paths, every pdtk_bench_* executable under
#   build/*/native/benchmarks/ is discovered and run.
#   --json DIR   additionally write each run's machine-readable JSON report
#                (full repetitions + aggregates) to DIR/<name>.json
#   --no-perf    skip the perf stat stage entirely
#
# Honest degradation (recorded as U-010 in docs/recovery/unknowns.md):
#   this script never fails because perf is missing or blocked by
#   kernel.perf_event_paranoid — the perf stage prints "SKIPPED (<reason>)"
#   and the latency benchmarking still completes. Missing PMU access must
#   not be misread as a benchmark failure. It DOES fail (exit 1) when a
#   benchmark binary itself fails, or when no benchmark binary is found.
#
# Recorded results live in docs/benchmarks/ (policy in that folder's README).

set -euo pipefail

PERF_EVENTS="L1-dcache-load-misses,instructions,cycles"
JSON_DIR=""
RUN_PERF=1
PATHS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --json)
            JSON_DIR="${2:?--json needs a directory argument}"
            mkdir -p "$JSON_DIR"
            shift 2
            ;;
        --no-perf)
            RUN_PERF=0
            shift
            ;;
        -h|--help)
            sed -n '2,24p' "$0"
            exit 0
            ;;
        *)
            PATHS+=("$1")
            shift
            ;;
    esac
done

# --- Resolve the benchmark set ---------------------------------------------
declare -a BINARIES=()
if [[ ${#PATHS[@]} -eq 0 ]]; then
    for candidate in build/*/native/benchmarks/pdtk_bench_*; do
        [[ -x "$candidate" ]] && BINARIES+=("$candidate")
    done
else
    for p in "${PATHS[@]}"; do
        if [[ -d "$p" ]]; then
            for candidate in "$p"/pdtk_bench_*; do
                [[ -x "$candidate" ]] && BINARIES+=("$candidate")
            done
        elif [[ -x "$p" ]]; then
            BINARIES+=("$p")
        else
            echo "run_perf.sh: not a benchmark binary or directory: $p" >&2
            exit 1
        fi
    done
fi

if [[ ${#BINARIES[@]} -eq 0 ]]; then
    echo "run_perf.sh: no benchmark binaries found." >&2
    echo "  Build them first:  cmake --preset release && cmake --build --preset release" >&2
    echo "  (benchmarks can be disabled per ADR-0005; pass explicit paths otherwise)" >&2
    exit 1
fi

echo "== benchmark stage: ${#BINARIES[@]} binary(ies) =="
for bin in "${BINARIES[@]}"; do
    echo "-- $bin"
    # Exactly ONE invocation per binary: console mode for humans, or JSON
    # mode for machines when --json was given. Two separate invocations
    # would be two independent measurements (run-to-run variance showed
    # ~0.3% mean drift between back-to-back runs — do not double-measure).
    if [[ -n "$JSON_DIR" ]]; then
        "$bin" --benchmark_format=json > "$JSON_DIR/$(basename "$bin").json" || exit 1
        echo "   report: $JSON_DIR/$(basename "$bin").json"
    else
        "$bin" || exit 1
    fi
done

# --- perf stat stage --------------------------------------------------------
if [[ $RUN_PERF -eq 0 ]]; then
    echo "== perf stage: SKIPPED (--no-perf) =="
    exit 0
fi
if ! command -v perf >/dev/null 2>&1; then
    echo "== perf stage: SKIPPED (perf not installed; see U-010) =="
    exit 0
fi
# Functional probe: containers and CI runners often ship perf but block it
# via kernel.perf_event_paranoid; a broken perf must not fail the run.
if ! perf stat -e instructions -- true >/dev/null 2>/tmp/pdtk_perf_probe.err; then
    echo "== perf stage: SKIPPED (perf installed but blocked: $(tr '\n' ' ' </tmp/pdtk_perf_probe.err | cut -c1-120)) =="
    exit 0
fi

echo "== perf stage: $PERF_EVENTS =="
for bin in "${BINARIES[@]}"; do
    echo "-- $bin (under perf stat)"
    perf stat -e "$PERF_EVENTS" "$bin" >/dev/null || exit 1
done
echo "== done =="
