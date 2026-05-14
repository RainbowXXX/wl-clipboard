# wayland-scanner integration: discovers the scanner via pkg-config or PATH,
# generates client header + private-code C files for each XML, and exposes
# a `wlproto` STATIC library that downstream targets can link against.

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
if(NOT WAYLAND_SCANNER)
    find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
endif()

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
target_include_directories(wlproto PUBLIC ${WL_PROTO_GEN_DIR})
target_link_libraries(wlproto PUBLIC PkgConfig::WAYLAND_CLIENT)

wlclip_add_protocol(
    ${CMAKE_SOURCE_DIR}/protocols/wlr-data-control-unstable-v1.xml
    wlr-data-control-unstable-v1
)
