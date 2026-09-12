const CACHE_PREFIX = `weather-clock-unified:${self.location.pathname}:`;
const SDL_PREVIEW_REVISION = "local";
const SIMULATOR_REVISION = "local";
const SIMULATOR_ASSETS = [];
const CACHE_NAME = `${CACHE_PREFIX}v64-${SDL_PREVIEW_REVISION}-${SIMULATOR_REVISION}`;
const ASSETS = [
  "./",
  "./index.html",
  "./styles.css",
  "./app.js",
  "./simulator-ui.js",
  "./quick-config.js",
  "./assets/icons/eye.svg",
  "./assets/icons/eye-off.svg",
  ...SIMULATOR_ASSETS,
  "./assets/screens/weather_clock_main.png",
  "./assets/screens/weather_clock_weather_board.png",
  "./assets/screens/weather_clock_gallery.png",
  "./assets/screens/weather_clock_flip_clock.png",
  "./assets/screens/weather_clock_history.png",
  "./assets/screens/weather_clock_calendar.png",
  "./assets/screens/weather_clock_xiaozhi.png",
  "./assets/screens/weather_clock_aggregate_clock.png",
  "./assets/weather_clock_main.png",
  "./assets/weather_clock_preview_sheet_1.png",
  "./vendor/esptool-js/0.5.6/bundle.js",
  "./vendor/esptool-js/0.5.6/LICENSE"
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then((cache) => cache.addAll(ASSETS))
      .catch(() => undefined)
  );
  self.skipWaiting();
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys().then((keys) => Promise.all(
      keys.filter((key) => key.startsWith(CACHE_PREFIX) && key !== CACHE_NAME).map((key) => caches.delete(key))
    ))
  );
  self.clients.claim();
});

self.addEventListener("fetch", (event) => {
  if (event.request.method !== "GET") return;
  const url = new URL(event.request.url);
  if (url.origin !== self.location.origin) return;
  if (/\/firmware\/releases(?:\/|\.json$)/.test(url.pathname)) {
    event.respondWith(fetch(event.request));
    return;
  }
  event.respondWith(
    caches.match(event.request).then((cached) => (
      cached || fetch(event.request).then((response) => {
        const copy = response.clone();
        caches.open(CACHE_NAME).then((cache) => cache.put(event.request, copy));
        return response;
      })
    ))
  );
});
