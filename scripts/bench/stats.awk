#!/usr/bin/env -S awk -f
# stats.awk — summarise distributions from clipboard-latency.sh
#
# Usage:
#   scripts/bench/stats.awk file1.log [file2.log ...]
#
# Each file is one sample per line (ms). Prints, per file:
#   N  min  p50  mean  p95  max  σ
#
# Also prints the delta of medians vs the first file, which is the gate
# we care about (wlclip-x11 vs xclip).

function percentile(a, n, p,   pos, lo, hi, frac) {
    pos = (p / 100) * (n - 1) + 1
    lo  = int(pos)
    hi  = lo + 1
    frac = pos - lo
    if (hi > n) hi = n
    return a[lo] + frac * (a[hi] - a[lo])
}

FNR == 1 {
    # New file — flush previous file's stats.
    if (file != "") emit()
    file = FILENAME
    n = 0
    sum = 0
    delete vals
}

/^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)$/ {
    n++
    vals[n] = $1 + 0
    sum += vals[n]
}

END { if (file != "") emit() }

function emit(   i, j, t, sq, mean, p50, p95, sd, min, max, delta) {
    if (n == 0) {
        printf "%-30s  (no samples)\n", file
        return
    }
    # Sort ascending — insertion sort, n is small.
    for (i = 2; i <= n; i++) {
        t = vals[i]
        j = i - 1
        while (j >= 1 && vals[j] > t) { vals[j+1] = vals[j]; j-- }
        vals[j+1] = t
    }
    mean = sum / n
    sq = 0
    for (i = 1; i <= n; i++) sq += (vals[i] - mean) * (vals[i] - mean)
    sd = (n > 1) ? sqrt(sq / (n - 1)) : 0
    min = vals[1]; max = vals[n]
    p50 = percentile(vals, n, 50)
    p95 = percentile(vals, n, 95)

    if (baseline_median == 0) {
        baseline_median = p50
        baseline_name = file
        printf "%-30s  N=%-3d  min=%6.1f  p50=%6.1f  mean=%6.1f  p95=%6.1f  max=%6.1f  σ=%.1f\n", \
               file, n, min, p50, mean, p95, max, sd
    } else {
        delta = p50 - baseline_median
        printf "%-30s  N=%-3d  min=%6.1f  p50=%6.1f  mean=%6.1f  p95=%6.1f  max=%6.1f  σ=%.1f   Δp50=%+.1f ms vs %s\n", \
               file, n, min, p50, mean, p95, max, sd, delta, baseline_name
    }
}
