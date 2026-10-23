"""Check that repeated scans keep returning named networks, strongest first."""
import json
import socket
import sys
import time
import urllib.request

BASE = "http://192.168.254.125"
TOKEN = sys.argv[1] if len(sys.argv) > 1 else "ec9b7a701ecba6c02a09448c646eb053"


def call(path, method="GET"):
    request = urllib.request.Request(BASE + path, method=method)
    request.add_header("Authorization", "Bearer " + TOKEN)
    with urllib.request.urlopen(request, timeout=25) as response:
        return response.read().decode()


for attempt in range(25):
    try:
        socket.create_connection(("192.168.254.125", 80), timeout=3).close()
        break
    except OSError:
        time.sleep(3)

for run in (1, 2):
    call("/api/wifi/scan", "POST")
    for _ in range(20):
        time.sleep(1.5)
        payload = json.loads(call("/api/wifi/scan"))
        if payload.get("running"):
            continue
        networks = [n for n in (payload.get("networks") or []) if n.get("ssid")]
        summary = ", ".join(f"{n['ssid']} {n['rssi']}dBm ch{n['channel']}" for n in networks[:5])
        print(f"scan {run}: {len(networks)} named of {payload.get('total')} -> {summary}")
        break
