"""Find routeOS boards on the LAN by probing the RVDP port."""
import socket
import sys
from concurrent.futures import ThreadPoolExecutor

port = int(sys.argv[1]) if len(sys.argv) > 1 else 81
base = sys.argv[2] if len(sys.argv) > 2 else "10.65.52."


def probe(host):
    sock = socket.socket()
    sock.settimeout(0.6)
    try:
        sock.connect((host, port))
        return host
    except OSError:
        return None
    finally:
        sock.close()


hosts = [f"{base}{n}" for n in range(1, 255)]
with ThreadPoolExecutor(max_workers=128) as pool:
    found = [h for h in pool.map(probe, hosts) if h]

print("open %d/tcp on: %s" % (port, ", ".join(found) if found else "nobody"))
