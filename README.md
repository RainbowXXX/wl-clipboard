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

### Cross-compiling for arm64 (aarch64)

```sh
# Debian/Ubuntu/UOS prerequisites:
sudo dpkg --add-architecture arm64
sudo apt update
sudo apt install \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    pkg-config wayland-scanner \
    libwayland-dev:arm64

# Build:
cmake -S . -B build-arm64 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-linux-gnu.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm64 -j
file build-arm64/wlclip   # → ELF 64-bit LSB executable, ARM aarch64
```

The toolchain file:
- Picks up `aarch64-linux-gnu-{gcc,g++,ar,ranlib,strip,ld}`. Override with
  `-DCROSS_TRIPLE=...` if your toolchain uses a different prefix.
- Forces `wayland-scanner` (a build-host code generator) to be located on
  PATH rather than in the target sysroot.
- Points `pkg-config` at `/usr/lib/aarch64-linux-gnu/pkgconfig` so
  `libwayland-client` resolves to the arm64 package.
- Pass `-DCROSS_SYSROOT=/path/to/sysroot` if you build against a sysroot
  instead of multiarch packages.

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
