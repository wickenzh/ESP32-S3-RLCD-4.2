# WeatherClock LVGL SDL Preview

This simulator previews the 400 x 300 LVGL clock UI on macOS with SDL2.

## Browser Target

Activate Emscripten 4.0.23 and run `python3 host_web/scripts/build_web_simulator.py`
from the repository root. The browser target uses LVGL 8.4.0 and SDL2, shares
the native preview renderers, and exposes a separate fake state adapter in
`web_demo_state.h`. It does not run ESP-IDF network, NVS, audio or flashing code.
The clock follows local computer time. K/B keyboard input and virtual buttons
drive settings and scenes; holding KEY for 1.2 seconds returns one level.

Generated JS, WASM, isolated portal HTML and hash metadata live in the ignored
`host_web/simulator/` directory. Pages rebuilds them from committed sources.
The build identity is embedded in WASM, not inferred from a remote latest tag.
Use `--lvgl` for a standalone LVGL 8.4.0 source directory. Production workflow
pins both the SDK commit and LVGL archive digest. Native golden previews retain
their separate fixed-time lifecycle.

Build from the firmware project directory:

```sh
cd <project-root>/RLCD_CLOCK
cmake -S simulator -B simulator/build
cmake --build simulator/build -j4
```

Run:

```sh
cd <project-root>/RLCD_CLOCK
./simulator/build/weather_clock_sdl
```

The preview window is scaled to 800 x 600, while the LVGL canvas remains the
same 400 x 300 size as the device. Press `Esc` or close the window to quit.

Module ownership:

- `main.cpp`: preview mode selection, shared page chrome, time progression and event loop.
- `sdl_preview_backend.*`: SDL resources, LVGL flush and PPM screenshot output.
- `sdl_preview_widgets.*`: shared labels, monochrome bars and canvas drawing primitives.
- `sdl_preview_settings.*`: settings-page previews.
- `sdl_preview_calendar.*`: calendar grid and lunar preview body.
- `sdl_preview_flip_cards.*`: shared inverted DSEG cards used by the temperature/humidity clock and Xiaozhi previews.
- `sdl_preview_flip_clock.*`: temperature/humidity clock sensor, mood and date preview body.
- `sdl_preview_gallery.*`: gallery image, block-clock and daily-saying preview body.
- `sdl_preview_history.*`: temperature/humidity history chart body.
- `sdl_preview_weather.*`: weather-board body and shared QWeather icon conversion.
- `sdl_preview_xiaozhi.*`: Xiaozhi dialogue, preparing and Pomodoro preview body.
