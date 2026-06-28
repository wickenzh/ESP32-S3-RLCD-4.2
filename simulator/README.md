# WeatherClock LVGL SDL Preview

This simulator previews the 400 x 300 LVGL clock UI on macOS with SDL2.

Build:

```sh
cd <project-root>/RLCD_CLOCK
cmake -S simulator -B build_sdl
cmake --build build_sdl -j4
```

Run:

```sh
cd <project-root>/RLCD_CLOCK
./build_sdl/weather_clock_sdl
```

The preview window is scaled to 800 x 600, while the LVGL canvas remains the
same 400 x 300 size as the device. Press `Esc` or close the window to quit.
