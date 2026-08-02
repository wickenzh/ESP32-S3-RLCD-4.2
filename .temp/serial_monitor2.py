"""Serial monitor - reads COM6 for 90 seconds, prints Radio/DNS/http/speaker related lines."""
import serial, time

keywords = ["Radio", "radio", "DNS", "dns", "gethost", "resolved", "http", "HTTP",
            "connect", "speaker", "MP3", "mp3", "audio", "playback", "sample rate",
            "codec", "write", "I2S", "error", "ERROR", "WARN", "warn"]
ser = serial.Serial("COM6", 115200, timeout=1)
start = time.time()
while time.time() - start < 90:
    try:
        line = ser.readline().decode("utf-8", errors="replace").rstrip()
        if not line:
            continue
        if any(k.lower() in line.lower() for k in ["radio", "dns", "resolv", "gethost",
            "http open", "http connect", "connect fail", "status=", "speaker", "mp3",
            "sample rate", "playback", "play write", "dec_", "esp_audio"]):
            print(line, flush=True)
    except Exception as e:
        print(f"ERR: {e}", flush=True)
ser.close()
