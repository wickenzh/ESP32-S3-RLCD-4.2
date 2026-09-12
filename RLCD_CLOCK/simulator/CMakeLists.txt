# 配置 LVGL SDL 模拟器的本地构建目标和依赖。
cmake_minimum_required(VERSION 3.16)
project(weather_clock_sdl_preview C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LV_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_INCLUDE_SIMPLE ON CACHE BOOL "" FORCE)
set(LV_LVGL_H_INCLUDE_SIMPLE ON CACHE BOOL "" FORCE)

set(WEATHER_CLOCK_LVGL_DIR "${CMAKE_CURRENT_LIST_DIR}/../managed_components/lvgl__lvgl" CACHE PATH "LVGL source directory")
add_subdirectory("${WEATHER_CLOCK_LVGL_DIR}" "${CMAKE_BINARY_DIR}/lvgl" EXCLUDE_FROM_ALL)

if(EMSCRIPTEN)
    set(SDL2_CFLAGS_OTHER "-sUSE_SDL=2")
    set(SDL2_LDFLAGS "-sUSE_SDL=2")
else()
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
endif()
if(NOT SDL2_FOUND)
    find_program(SDL2_CONFIG_EXECUTABLE NAMES sdl2-config REQUIRED)
    execute_process(
        COMMAND "${SDL2_CONFIG_EXECUTABLE}" --cflags
        OUTPUT_VARIABLE SDL2_CFLAGS_TEXT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE SDL2_CFLAGS_RESULT)
    execute_process(
        COMMAND "${SDL2_CONFIG_EXECUTABLE}" --libs
        OUTPUT_VARIABLE SDL2_LDFLAGS_TEXT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE SDL2_LDFLAGS_RESULT)
    if(NOT SDL2_CFLAGS_RESULT EQUAL 0 OR NOT SDL2_LDFLAGS_RESULT EQUAL 0)
        message(FATAL_ERROR "sdl2-config failed to provide compiler or linker flags")
    endif()
    separate_arguments(SDL2_CFLAGS_OTHER NATIVE_COMMAND "${SDL2_CFLAGS_TEXT}")
    separate_arguments(SDL2_LDFLAGS NATIVE_COMMAND "${SDL2_LDFLAGS_TEXT}")
    set(SDL2_INCLUDE_DIRS "")
endif()
endif()

add_executable(weather_clock_sdl
    "main.cpp"
    "sdl_preview_backend.cpp"
    "sdl_preview_boot.cpp"
    "sdl_preview_calendar.cpp"
    "sdl_preview_clock.cpp"
    "sdl_preview_flip_cards.cpp"
    "sdl_preview_flip_clock.cpp"
    "sdl_preview_gallery.cpp"
    "sdl_preview_history.cpp"
    "sdl_preview_progress.cpp"
    "sdl_preview_settings.cpp"
    "sdl_preview_weather.cpp"
    "sdl_preview_widgets.cpp"
    "sdl_preview_work_status.cpp"
    "sdl_preview_xiaozhi.cpp"
    "../main/ui/ui_dseg_render.cpp"
    "../main/ui/ui_aggregate_clock_view.cpp"
    "../main/network/qweather_icons.cpp"
    "../main/assets/boot_anim.c"
    "../main/assets/qweather_icons_36.c"
    "../main/assets/zh_font_16.c"
    "../main/assets/zh_flip_lunar_22.c"
    "../main/assets/aggregate_numeric_20.c"
    "../main/assets/zh_pomodoro_title_24.c")

target_include_directories(weather_clock_sdl PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "../main"
    "../main/assets"
    "../main/ui"
    "../main/network"
    "../main/core"
    ${SDL2_INCLUDE_DIRS})

target_compile_options(weather_clock_sdl PRIVATE ${SDL2_CFLAGS_OTHER})
target_link_options(weather_clock_sdl PRIVATE ${SDL2_LDFLAGS})
target_link_libraries(weather_clock_sdl PRIVATE lvgl)

if(EMSCRIPTEN)
    target_compile_definitions(weather_clock_sdl PRIVATE WEATHER_CLOCK_WEB_SIMULATOR=1)
    target_include_directories(weather_clock_sdl PRIVATE "${CMAKE_BINARY_DIR}")
    target_link_options(weather_clock_sdl PRIVATE
        "-sMODULARIZE=1" "-sEXPORT_ES6=1" "-sEXPORT_NAME=createWeatherClock"
        "-sALLOW_MEMORY_GROWTH=1" "-sINITIAL_MEMORY=33554432" "-sMAXIMUM_MEMORY=134217728"
        "-sENVIRONMENT=web" "-sNO_EXIT_RUNTIME=1"
        "-sEXPORTED_FUNCTIONS=['_main','_demo_tick','_demo_key','_demo_scene','_demo_reset','_demo_state','_demo_build_info']"
        "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString']")
    set_target_properties(weather_clock_sdl PROPERTIES OUTPUT_NAME "weather-clock" SUFFIX ".js")
endif()

if(NOT EMSCRIPTEN)
add_executable(web_demo_state_test "web_demo_state_test.cpp")
target_include_directories(web_demo_state_test PRIVATE "../main/core" "../main/ui")

add_executable(aggregate_clock_view_test
    "../main/assets/aggregate_numeric_20.c"
    "../main/assets/zh_flip_lunar_22.c"
    "../tests/aggregate_clock_view_host_test.cpp"
    "../main/ui/ui_aggregate_clock_view.cpp"
    "../main/assets/zh_font_16.c"
    "../main/assets/qweather_icons_36.c")
target_include_directories(aggregate_clock_view_test PRIVATE "../main/ui" "../main/assets")
target_link_libraries(aggregate_clock_view_test PRIVATE lvgl)

add_executable(settings_visual_style_test "../tests/settings_visual_style_host_test.cpp")
target_include_directories(settings_visual_style_test PRIVATE "../main/ui")
target_link_libraries(settings_visual_style_test PRIVATE lvgl)
endif()
