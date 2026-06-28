# 配置 LVGL SDL 模拟器的本地构建目标和依赖。
cmake_minimum_required(VERSION 3.16)
project(weather_clock_sdl_preview C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LV_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_INCLUDE_SIMPLE ON CACHE BOOL "" FORCE)
set(LV_LVGL_H_INCLUDE_SIMPLE ON CACHE BOOL "" FORCE)

add_subdirectory("../managed_components/lvgl__lvgl" "${CMAKE_BINARY_DIR}/lvgl" EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(weather_clock_sdl
    "main.cpp"
    "../main/assets/boot_anim.c"
    "../main/assets/qweather_icons_36.c"
    "../main/assets/zh_font_16.c")

target_include_directories(weather_clock_sdl PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "../main"
    "../main/assets"
    ${SDL2_INCLUDE_DIRS})

target_compile_options(weather_clock_sdl PRIVATE ${SDL2_CFLAGS_OTHER})
target_link_options(weather_clock_sdl PRIVATE ${SDL2_LDFLAGS})
target_link_libraries(weather_clock_sdl PRIVATE lvgl)
