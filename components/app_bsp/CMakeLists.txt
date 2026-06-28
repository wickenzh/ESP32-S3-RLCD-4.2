# 配置 LVGL 应用 BSP 组件的源码和依赖。
idf_component_register(
    SRCS
    "lvgl_bsp.cpp"
    PRIV_REQUIRES 
    port_bsp
    esp_timer
    REQUIRES
    lvgl__lvgl
    INCLUDE_DIRS 
    "./")
