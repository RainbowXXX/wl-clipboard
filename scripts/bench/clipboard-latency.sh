#!/usr/bin/env bash
# clipboard-latency.sh — measure copy→paste user-perceived latency
#
# Sets a fresh, unique TAG via the chosen "copy tool", then polls a fresh
# `wlclip -P raw paste` until the new TAG becomes visible on the wayland
# data_control device. Repeats N times and prints the per-run latencies
# (ms) one per line — suitable for piping into sort | awk for stats.
#
# Usage:
#   clipboard-latency.sh <tool> [runs] [poll-interval-s] [poll-timeout-s]
#
#   tool: xclip | wlclip-x11 | wlclip-wlr | wlclip-handcrafted
#   runs: number of samples (default 20)
#   poll-interval-s: sleep between paste polls (default 0.005)
#   poll-timeout-s:  per-run give-up timeout (default 2)
#
# Example:
#   scripts/bench/clipboard-latency.sh xclip 20 > xclip.log
#   scripts/bench/clipboard-latency.sh wlclip-x11 20 > wlclip-x11.log
#   scripts/bench/stats.awk xclip.log wlclip-x11.log
#
# The acceptance gate: wlclip-x11 median MUST stay within
#   xclip_median + 10ms
# regressing past that means someone re-bloated the advertised TARGETS
# list (see clipboard/x11.cpp:build_targets — the comment there explains
# why short matters).

set -u

WLCLIP_BIN="${WLCLIP_BIN:-$(dirname "$0")/../../build/wlclip}"

tool="${1:-}"
runs="${2:-20}"
poll_int="${3:-0.005}"
poll_to="${4:-2}"

if [[ -z "$tool" ]]; then
    echo "usage: $0 <xclip|wlclip-x11|wlclip-wlr|wlclip-handcrafted> [runs] [poll-int-s] [poll-timeout-s]" >&2
    exit 2
fi

if [[ ! -x "$WLCLIP_BIN" ]]; then
    echo "wlclip not found at $WLCLIP_BIN (set WLCLIP_BIN= to override)" >&2
    exit 2
fi

case "$tool" in
    xclip)
        command -v xclip >/dev/null || { echo "xclip not installed" >&2; exit 2; }
        do_copy() { printf '%s' "$1" | xclip -selection clipboard -i; }
        ;;
    wlclip-x11)         do_copy() { "$WLCLIP_BIN" -P x11    copy        "$1" 2>/dev/null; } ;;
    wlclip-x11-sync)    do_copy() { "$WLCLIP_BIN" -P x11    copy --sync "$1" 2>/dev/null; } ;;
    wlclip-wlr)         do_copy() { "$WLCLIP_BIN" -P wlr    copy        "$1" 2>/dev/null; } ;;
    wlclip-handcrafted) do_copy() { "$WLCLIP_BIN" -P raw    copy        "$1" 2>/dev/null; } ;;
    *) echo "unknown tool: $tool" >&2; exit 2 ;;
esac

# Time deadline arithmetic in pure awk so we don't depend on `date +%s.%N`
# behaving identically across systems.
elapsed_ms_since() {
    local t0="$1"
    local t1
    t1=$(date +%s.%N)
    awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f\n", (b - a) * 1000 }'
}

for run in $(seq 1 "$runs"); do
    TAG="BENCH_$(date +%s%N)_${run}_$$"

    do_copy "$TAG"

    T0=$(date +%s.%N)
    DEADLINE=$(awk -v t="$T0" -v to="$poll_to" 'BEGIN { printf "%.6f\n", t + to }')

    hit=0
    while :; do
        now=$(date +%s.%N)
        if awk -v n="$now" -v d="$DEADLINE" 'BEGIN { exit !(n > d) }'; then
            break
        fi
        v=$("$WLCLIP_BIN" -P raw paste 2>/dev/null || true)
        # xclip-injected text has a trailing newline because of `echo`;
        # wlclip copy <arg> also appends one. Strip from both ends before
        # compare so the two tools are commensurable.
        v="${v%$'\n'}"
        if [[ "$v" == "$TAG" ]]; then
            elapsed_ms_since "$T0"
            hit=1
            break
        fi
        sleep "$poll_int"
    done

    if [[ "$hit" -eq 0 ]]; then
        echo "TIMEOUT" >&2
        # Emit NaN so downstream stats can decide whether to drop or count.
        echo "nan"
    fi

    # Brief breather between runs so each iteration starts from a similar
    # clipboard-manager state. Without this, the manager's commit timer
    # from run N can bleed into run N+1's measurement.
    sleep 0.2
done
