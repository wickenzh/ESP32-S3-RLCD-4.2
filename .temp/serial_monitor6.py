"""Serial monitor - 180s, focus on radio playback"""
import serial, time

ser = serial.Serial("COM6", 115200, timeout=1)
start = time.time()
while time.time() - start < 180:
    try:
        line = ser.readline().decode("utf-8", errors="replace").rstrip()
        if not line:
            continue
        safe = line.encode("ascii", errors="replace").decode("ascii")
        # Print radio-related and key system logs
        if any(k in safe for k in ["Radio", "radio", "MP3", "mp3", "speaker",
            "codec", "Codec", "I2S", "i2s", "wifi radio", "wifi stop",
            "switch work page", "page activ", "page deactiv",
            "socket", "Socket", "connect", "HTTP/",
            "decode", "sync", "PCM", "pcm", "sample_rate",
            "playback", "error", "Error", "errno"]):
            print(safe, flush=True)
    except KeyboardInterrupt:
        break
    except Exception:
        pass
ser.close()
