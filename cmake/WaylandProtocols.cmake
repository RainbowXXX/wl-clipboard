# wayland-scanner integration: discovers the scanner via pkg-config or PATH,
# generates client header + private-code C files for each XML, and exposes
# a `wlproto` STATIC library that downstream targets can link against.

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

# wayland-scanner is a BUILD-machine tool. When cross-compiling, the cross
# pkg-config would point at the target sysroot's binary which won't run on
# the host, so we look it up on PATH first and only fall back to pkg-config.
find_program(WAYLAND_SCANNER NAMES wayland-scanner)
if(NOT WAYLAND_SCANNER)
    pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
endif()
if(NOT WAYLAND_SCANNER)
    message(FATAL_ERROR
        "wayland-scanner not found. Install it on the build machine "
        "(e.g. 'apt install wayland-scanner' on Debian/Ubuntu) and ensure "
        "it is on PATH.")
endif()
message(STATUS "wayland-scanner: ${WAYLAND_SCANNER}")

set(WL_PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${WL_PROTO_GEN_DIR})

function(wlclip_add_protocol xml_path out_basename)
    set(hdr ${WL_PROTO_GEN_DIR}/${out_basename}-client-protocol.h)
    set(src ${WL_PROTO_GEN_DIR}/${out_basename}-protocol.c)
    add_custom_command(
        OUTPUT ${hdr}
        COMMAND ${WAYLAND_SCANNER} client-header ${xml_path} ${hdr}
        DEPENDS ${xml_path}
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${src}
        COMMAND ${WAYLAND_SCANNER} private-code ${xml_path} ${src}
        DEPENDS ${xml_path}
        VERBATIM
    )
    target_sources(wlproto PRIVATE ${src} ${hdr})
endfunction()

add_library(wlproto STATIC)
set_target_properties(wlproto PROPERTIES LINKER_LANGUAGE C)
target_include_directories(wlproto PUBLIC ${WL_PROTO_GEN_DIR})
target_link_libraries(wlproto PUBLIC PkgConfig::WAYLAND_CLIENT)

wlclip_add_protocol(
    ${CMAKE_SOURCE_DIR}/protocols/wlr-data-control-unstable-v1.xml
    wlr-data-control-unstable-v1
)
