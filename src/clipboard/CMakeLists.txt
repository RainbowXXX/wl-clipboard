add_library(wlclip_clipboard STATIC
    transfer.cpp
    data_control.cpp
    data_device.cpp
    factory.cpp
)
target_include_directories(wlclip_clipboard PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(wlclip_clipboard
    PUBLIC  wlclip_wayland wlclip_core wlproto
)
