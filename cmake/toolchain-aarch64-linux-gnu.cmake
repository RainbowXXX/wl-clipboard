# Cross-compile toolchain for aarch64 (arm64) Linux glibc.
#
# Usage:
#   cmake -S . -B build-arm64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-linux-gnu.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-arm64 -j
#
# Requirements on the build host (Debian/Ubuntu/UOS):
#   apt install \
#       gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
#       pkg-config wayland-scanner \
#       libwayland-dev:arm64
#
#   # And enable the arm64 architecture if you haven't:
#   dpkg --add-architecture arm64 && apt update
#
# wayland-scanner runs on the BUILD machine (it generates code), while
# libwayland-client must be the ARM64 build. The PKG_CONFIG_* env vars
# below scope pkg-config to the cross sysroot for wayland-client.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Override with -DCROSS_TRIPLE=... if your toolchain has a different prefix.
if(NOT DEFINED CROSS_TRIPLE)
    set(CROSS_TRIPLE aarch64-linux-gnu)
endif()

set(CMAKE_C_COMPILER   ${CROSS_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${CROSS_TRIPLE}-g++)
set(CMAKE_AR           ${CROSS_TRIPLE}-ar          CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       ${CROSS_TRIPLE}-ranlib      CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP        ${CROSS_TRIPLE}-strip       CACHE FILEPATH "" FORCE)
set(CMAKE_LINKER       ${CROSS_TRIPLE}-ld          CACHE FILEPATH "" FORCE)

# Optional sysroot (override with -DCROSS_SYSROOT=/path/to/sysroot).
if(DEFINED CROSS_SYSROOT)
    set(CMAKE_SYSROOT ${CROSS_SYSROOT})
    set(CMAKE_FIND_ROOT_PATH ${CROSS_SYSROOT})
endif()

# Look for headers/libs/packages in the target sysroot, but find PROGRAMS
# (compilers, wayland-scanner, pkg-config) on the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Point pkg-config at the arm64 multiarch directories. If you use a custom
# sysroot, set PKG_CONFIG_LIBDIR / PKG_CONFIG_SYSROOT_DIR in your env
# instead of relying on these defaults.
if(NOT DEFINED ENV{PKG_CONFIG_LIBDIR} AND NOT DEFINED CROSS_SYSROOT)
    set(ENV{PKG_CONFIG_LIBDIR}
        "/usr/lib/${CROSS_TRIPLE}/pkgconfig:/usr/share/pkgconfig")
endif()
if(NOT DEFINED ENV{PKG_CONFIG_PATH})
    set(ENV{PKG_CONFIG_PATH} "")
endif()
