"""Serial monitor for Radio - 180s, watch for decoder+playback"""
import serial, time, sys

ser = serial.Serial("COM6", 115200, timeout=1)
start = time.time()
while time.time() - start < 180:
    try:
        line = ser.readline().decode("utf-8", errors="replace").rstrip()
        if not line:
            continue
        kl = line.lower()
        if any(k in kl for k in ["radio", "dns", "resolv", "gethost",
            "http open", "connect fail", "speaker", "mp3", "sample rate",
            "playback", "dec_", "esp_audio", "decoder", "register",
            "skip", "keepalive", "page activated", "page deactiv",
            "switch work page", "wifi radio off", "wifi radio on",
            "codec", "i2s", "audio dec", "play write"]):
            print(line, flush=True)
    except Exception as e:
        print(f"ERR: {e}", flush=True)
ser.close()
