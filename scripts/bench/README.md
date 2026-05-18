# Clipboard latency bench

Measures the end-to-end *user-perceived* latency from "copy tool returned" to
"a fresh paste sees the new value on the wayland data_control device".

## Quick run

```bash
cmake --build build -t wlclip
scripts/bench/clipboard-latency.sh xclip       20 > /tmp/xclip.log
scripts/bench/clipboard-latency.sh wlclip-x11  20 > /tmp/wlclip-x11.log
scripts/bench/stats.awk /tmp/xclip.log /tmp/wlclip-x11.log
```

Expected output (numbers are KDE/dde-clipboard, arm64, May 2026):

```
/tmp/xclip.log         N=20  min=  87.2  p50=  97.1  mean=  97.6  p95= 113.8  max= 118.8  σ=8.7
/tmp/wlclip-x11.log    N=20  min=  91.0  p50=  98.8  mean=  99.9  p95= 112.7  max= 113.4  σ=7.5   Δp50=+1.7 ms vs /tmp/xclip.log
```

## Acceptance gate

`wlclip-x11` median **must** stay within `xclip_median + 10 ms`. If a change
regresses past that, the most likely culprit is `clipboard/x11.cpp` —
specifically `build_targets`. The comment there explains why we advertise
*only* `{TARGETS, UTF8_STRING}` for text: every extra advertised target adds
one synchronous round-trip to the clipboard manager's internal
"verify-then-publish" pipeline. Going from 2 advertised atoms to 4 was
measured at +90 ms on KDE.

## What it measures

```
   user runs `wlclip -P x11 copy "$TAG"`
                │
                ▼
   wlclip parent forks, parent _exit(0)             ─┐
                │                                    │
                ▼                                    │
   T0 := date +%s.%N                                 │
                │                                    │ measured here
                ▼                                    │
   loop: wlclip -P raw paste; sleep 0.005           │
         until paste output == "$TAG"               │
                │                                    │
                ▼                                    │
   T1 := date +%s.%N                                ─┘
   emit (T1 - T0) ms
```

So the number includes:
- propagation through Xwayland / kwin / klipper-or-dde-clipboard
- one full `wlclip raw paste` process startup per poll (~3-10 ms each)
- 5 ms sleep granularity

The acceptance comparison against xclip is fair because both runs share all
of the above except the copy tool itself.

## Tools you can compare

| name                  | what it does                                          |
|-----------------------|-------------------------------------------------------|
| `xclip`               | `xclip -selection clipboard -i`                       |
| `wlclip-x11`          | `wlclip -P x11 copy <tag>` — what we actually ship    |
| `wlclip-x11-sync`     | `wlclip -P x11 copy --sync <tag>` — copy returns only after a wayland paste of the new value is verified end-to-end. Total wall time should be slightly *higher* than `wlclip-x11`, but the first paste after copy returns is guaranteed to succeed (the poll loop in this script will hit on iteration #1). |
| `wlclip-wlr`          | `wlclip -P wlr copy <tag>` — pure wayland path        |
| `wlclip-handcrafted`  | `wlclip -P raw copy <tag>` — pure wayland, no libwayland |

## Environment knobs

- `WLCLIP_BIN`: path to the wlclip binary (default `../../build/wlclip`
  relative to this script).
- runs / poll-interval / per-run timeout via positional args.

## What the numbers mean

`p50` is the headline number. `mean` should track `p50` closely if the
distribution isn't pathologically tailed. `p95` catches the "klipper /
dde-clipboard happened to be mid-commit" outliers — if `p95` is *much*
higher than `p50` for `wlclip-x11` but not for `xclip`, the clipboard
manager is treating us specially again and `build_targets` is the first
place to look.

## Why a wrapper script and not a CTest target

These numbers depend on a running session (xwayland, kwin, a clipboard
manager). They can't run in CI. They're a developer-machine sanity check
when touching the X11 backend's TARGETS list or selection-ownership flow.
