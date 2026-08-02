"""Serial monitor for Radio - reads COM6 for 100 seconds."""
import serial, time, sys

ser = serial.Serial("COM6", 115200, timeout=1)
start = time.time()
while time.time() - start < 100:
    try:
        line = ser.readline().decode("utf-8", errors="replace").rstrip()
        if not line:
            continue
        kl = line.lower()
        if any(k in kl for k in ["radio", "dns", "resolv", "gethost", "wifi",
            "http open", "http connect", "connect fail", "status=", "speaker",
            "mp3", "sample rate", "playback", "play write", "dec_",
            "esp_audio", "codec", "i2s", "skip", "keepalive", "network"]):
            print(line, flush=True)
    except Exception as e:
        print(f"ERR: {e}", flush=True)
ser.close()
