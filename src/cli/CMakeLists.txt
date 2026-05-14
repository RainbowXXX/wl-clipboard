add_library(wlclip_cli STATIC
    options.cpp
    copy.cpp
    paste.cpp
    protocols.cpp
)
target_include_directories(wlclip_cli PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(wlclip_cli
    PUBLIC  wlclip_clipboard wlclip_wayland wlclip_core wlproto
)
