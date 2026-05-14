add_library(wlclip_wire STATIC
    wire.cpp
)
target_include_directories(wlclip_wire PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(wlclip_wire PUBLIC wlclip_core)
