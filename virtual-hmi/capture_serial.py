"""Capture prototype diagnostics without sharing serial with a flashing process."""
import argparse
from pathlib import Path
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='COM4')
parser.add_argument('--seconds', type=int, default=30)
parser.add_argument('--reset', action='store_true')
parser.add_argument('--output', type=Path, default=Path('build/hmi-serial.log'))
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
with serial.Serial(args.port, 115200, timeout=0.2) as port:
    port.dtr = False
    port.rts = False
    if args.reset:
        port.rts = True
        time.sleep(0.1)
        port.rts = False
    deadline = time.monotonic() + args.seconds
    with args.output.open('w', encoding='utf-8') as log:
        while time.monotonic() < deadline:
            line = port.readline().decode('utf-8', 'replace')
            if line:
                log.write(line)
                log.flush()
                if '[hmi]' in line:
                    print(line.rstrip(), flush=True)
