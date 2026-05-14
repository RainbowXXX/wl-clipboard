add_library(wlclip_core STATIC
    log.cpp
    fd.cpp
    mime.cpp
)
target_include_directories(wlclip_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(wlclip_core PUBLIC spdlog::spdlog)
