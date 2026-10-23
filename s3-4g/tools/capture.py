import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
reset = len(sys.argv) > 3 and sys.argv[3] == "reset"

s = serial.Serial(port, 115200, timeout=0.2)
if reset:
    # Mirror esptool's hard reset so the capture starts at the boot banner.
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.15)
    s.setRTS(False)
    time.sleep(0.05)
end = time.time() + seconds
buf = b""
while time.time() < end:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
        sys.stdout.buffer.write(chunk)
        sys.stdout.flush()
s.close()
