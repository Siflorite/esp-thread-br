#!/usr/bin/env python3
"""Drive the border router console over USB serial: wait for boot, connect Wi-Fi.

Usage: serial_connect.py [port] [ssid] [password]
"""
import sys, time, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM1"
SSID = sys.argv[2] if len(sys.argv) > 2 else "otcisrp"
PSK = sys.argv[3] if len(sys.argv) > 3 else "espressif"

s = serial.Serial(PORT, 115200, timeout=0.3)
s.setDTR(False)
s.setRTS(False)
time.sleep(0.2)
s.reset_input_buffer()

deadline = time.time() + 40
buf = b""
while time.time() < deadline:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()
    # The CLI prints "> " when it is ready for a command.
    if buf.rstrip().endswith(b">"):
        break

print("\n=== sending wifi connect ===", flush=True)
s.write(("ot wifi connect -s %s -p %s\r\n" % (SSID, PSK)).encode())
s.flush()

end = time.time() + 45
buf = b""
while time.time() < end:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()
        if b"successfully" in buf or b"connected successfully" in buf:
            break
        if b"failed" in buf and b"connection is failed" in buf:
            break

print("\n=== done ===", flush=True)
s.close()
