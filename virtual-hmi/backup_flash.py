"""Read and verify a recoverable 4-MiB lab-board backup without writing flash.

Chunked reads tolerate USB adapters that corrupt long high-baud transfers.
Backups may contain Wi-Fi secrets: keep output in ignored build/hmi-backup.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', default='COM4')
parser.add_argument('--output', type=Path, default=Path('build/hmi-backup/original-COM4-20261022.bin'))
args = parser.parse_args()
if args.output.exists():
    raise SystemExit('Backup already exists; preserved. Choose a different --output for a new capture.')
args.output.parent.mkdir(parents=True, exist_ok=True)
chunk_size = 256 * 1024
chunks = []
for offset in range(0, 4 * 1024 * 1024, chunk_size):
    part = args.output.with_suffix(f'.{offset:06x}.part')
    if not part.exists() or part.stat().st_size != chunk_size:
        for baud in (460800, 115200):
            command = [sys.executable, '-m', 'esptool', '--port', args.port, '--baud', str(baud),
                       'read-flash', hex(offset), hex(chunk_size), str(part)]
            result = subprocess.run(command, capture_output=True, timeout=100)
            part.with_suffix('.log').write_bytes(result.stdout + result.stderr)
            if result.returncode == 0 and part.exists() and part.stat().st_size == chunk_size:
                break
        else:
            # The board's stub can corrupt bulk reads even at low baud. ROM reads
            # are slower and are verified against the device's independent MD5.
            command = [sys.executable, '-m', 'esptool', '--no-stub', '--port', args.port,
                       '--baud', '460800', 'read-flash', hex(offset), hex(chunk_size), str(part)]
            result = subprocess.run(command, capture_output=True, timeout=180)
            part.with_suffix('.rom.log').write_bytes(result.stdout + result.stderr)
            if result.returncode != 0:
                raise SystemExit(f'ROM backup failed at {offset:#x}; no firmware has been written.')
            verify = subprocess.run([sys.executable, '-m', 'esptool', '--no-stub', '--port', args.port,
                                     'verify-flash', hex(offset), str(part)], capture_output=True, timeout=60)
            part.with_suffix('.verify.log').write_bytes(verify.stdout + verify.stderr)
            if verify.returncode != 0:
                part.rename(part.with_suffix('.unverified'))
                raise SystemExit(f'Backup verification failed at {offset:#x}; no firmware has been written.')
    chunks.append(part.read_bytes())
    print(f'Backed up {offset + chunk_size:#x}/0x400000', flush=True)
data = b''.join(chunks)
args.output.write_bytes(data)
verify = subprocess.run([sys.executable, '-m', 'esptool', '--port', args.port,
                         'verify-flash', '0', str(args.output)], capture_output=True, timeout=60)
args.output.with_suffix('.verify-complete.log').write_bytes(verify.stdout + verify.stderr)
if verify.returncode != 0:
    raise SystemExit('Whole-image verification failed; backup is not approved for rollback.')
digest = hashlib.sha256(data).hexdigest()
args.output.with_suffix('.sha256').write_text(digest + '\n')
print(f'Complete backup: {len(data)} bytes; SHA256 {digest}', flush=True)
