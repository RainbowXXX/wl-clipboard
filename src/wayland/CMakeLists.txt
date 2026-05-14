add_library(wlclip_wayland STATIC
    state.cpp
)
target_include_directories(wlclip_wayland PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(wlclip_wayland
    PUBLIC  wlclip_core wlproto
)
