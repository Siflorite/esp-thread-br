#!/usr/bin/env python3
"""Send a console command over USB serial and print the response.

Usage: serial_cmd.py <port> <command...>
"""
import sys, time, serial

PORT = sys.argv[1]
CMD = " ".join(sys.argv[2:])

s = serial.Serial(PORT, 115200, timeout=0.3)
s.setDTR(False)
s.setRTS(False)
time.sleep(0.2)
s.reset_input_buffer()

# Wait for a prompt (either "esp32s3>" or the OT CLI ">").
deadline = time.time() + 15
buf = b""
while time.time() < deadline:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
    if buf.rstrip().endswith(b">"):
        break

print("=== sending: %s ===" % CMD, flush=True)
s.write((CMD + "\r\n").encode())
s.flush()

end = time.time() + 20
buf = b""
while time.time() < end:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
        if buf.rstrip().endswith(b">") and len(buf) > len(CMD) + 8:
            break

sys.stdout.write(buf.decode("utf-8", "replace"))
print("\n=== end ===", flush=True)
s.close()
