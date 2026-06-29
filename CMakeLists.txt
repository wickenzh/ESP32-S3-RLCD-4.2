# 配置天气时钟 ESP-IDF 工程版本、项目名和额外组件目录。
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "v1.4.37")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS
    components/ExternLib/SensorLib
    components/ExternLib/esp_codec_dev
    components/ExternLib/codec_board)
project(weather_clock)
