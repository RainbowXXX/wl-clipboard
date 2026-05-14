# wlclip

Minimal Wayland clipboard tool — a single binary with `copy`, `paste`, and
`protocols` subcommands.

## Supported protocols

Only two clipboard protocols are bound:

1. **`zwlr_data_control_unstable_v1`** (preferred) — works without input focus.
2. **`wl_data_device_manager`** (fallback) — implemented **without** the
   layer-shell window / focus-grab hack used by upstream `wl-clipboard`.
   Because core data devices require a keyboard-focus serial, this backend
   only works where focus already exists. On normal desktops it logs a
   warning and fails rather than hanging.

Logging is done via [spdlog](https://github.com/gabime/spdlog), vendored as a
git submodule under `3rdparty/spdlog`.

## Source layout

```
src/
├── main.cpp                  argv routing only
├── core/                     logging, FD/Pipe RAII, MIME helpers
├── wayland/                  wl_display + registry + seat + manager binding
├── clipboard/                Backend interface, transfer helper, two protocols
└── cli/                      global option parser + subcommands
```

Each module is a self-contained static library; the final `wlclip`
executable links them together.

## Build

```sh
git clone <this-repo>
cd wlclip
git submodule update --init --recursive   # fetch 3rdparty/spdlog
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/src/wlclip --help
```

Options:

- `-DWLCLIP_STATIC=ON` — pass `-static-libstdc++ -static-libgcc` to the
  final link. libwayland-client is usually shared-only on distros; pass full
  `-static` via `LDFLAGS` if you have a static build available.
- `-DWLCLIP_USE_SYSTEM_SPDLOG=ON` — use a system spdlog instead of the
  submodule copy.

## Usage

```sh
# Copy
echo hello | wlclip copy
wlclip copy -t text/plain "some text"
wlclip copy -c                # clear selection (requires wlr_data_control)
wlclip --primary copy < file  # primary selection

# Paste
wlclip paste                  # write current selection to stdout
wlclip paste -l               # list offered MIME types
wlclip paste -t image/png > clip.png

# Inspect compositor globals
wlclip protocols
wlclip protocols -v           # tab-separated with version + global id
```

Increase logging with `-v` / `-vv`, silence with `-q`.

## License

MIT.
