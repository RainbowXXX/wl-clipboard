add_subdirectory(core)
add_subdirectory(wayland)
add_subdirectory(clipboard)
add_subdirectory(cli)

add_executable(wlclip main.cpp)
target_link_libraries(wlclip PRIVATE wlclip_cli)

if(WLCLIP_STATIC)
    target_link_options(wlclip PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS wlclip RUNTIME DESTINATION bin)
