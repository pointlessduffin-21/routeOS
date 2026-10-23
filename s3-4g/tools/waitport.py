"""Poll the RVDP port over Tailscale until it answers, then measure the stream."""
import socket
import sys
import time

host = sys.argv[1] if len(sys.argv) > 1 else "100.70.23.100"
deadline = time.time() + float(sys.argv[2] if len(sys.argv) > 2 else 90)

attempt = 0
while time.time() < deadline:
    attempt += 1
    started = time.time()
    try:
        sock = socket.create_connection((host, 81), timeout=6)
        sock.close()
        print(f"attempt {attempt}: CONNECTED in {time.time() - started:.2f}s")
        sys.exit(0)
    except Exception as exc:
        print(f"attempt {attempt}: {type(exc).__name__} after {time.time() - started:.2f}s")
    time.sleep(3)

print("gave up")
sys.exit(1)
