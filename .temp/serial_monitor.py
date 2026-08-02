"""Simple serial monitor - reads COM6 for 60 seconds, prints lines containing Radio/DNS keywords."""
import serial, time, sys

keywords = ["Radio", "radio", "DNS", "dns", "getaddr", "resolved", "http", "HTTP", "connect", "speaker", "MP3", "mp3", "audio", "playback"]
ser = serial.Serial("COM6", 115200, timeout=1)
start = time.time()
while time.time() - start < 60:
    try:
        line = ser.readline().decode("utf-8", errors="replace").rstrip()
        if not line:
            continue
        # Print all lines that contain any keyword
        if any(k in line for k in keywords):
            print(line, flush=True)
    except Exception as e:
        print(f"ERR: {e}", flush=True)
ser.close()
