"""Repeat complete hardware sessions and verify malformed/unauthorized rejection.

Use iShareScreen's Python so framebuffer validation is the actual native codec.
"""
import argparse
import asyncio
import json
from pathlib import Path
import time
import aiohttp
from isharescreen.protocols.rvdp import wire as W
from isharescreen.protocols.rvdp.client import Framebuffer

parser = argparse.ArgumentParser()
parser.add_argument('--url', default='ws://192.168.254.123:81/rvdp')
parser.add_argument('--health', default='http://192.168.254.123/health')
parser.add_argument('--sessions', type=int, default=100)
parser.add_argument('--seconds', type=int, default=0, help='continuous receive after reconnect test')
parser.add_argument('--output', type=Path, default=Path('build/hmi-stress.json'))
args = parser.parse_args()

async def screen(ws):
    f = Framebuffer()
    tx = 1
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        msg = await ws.receive(timeout=5)
        if msg.type != aiohttp.WSMsgType.BINARY:
            raise AssertionError(f'Closed before full screen: {msg.type}')
        event, _ = f.receive(msg.data)
        if event == 'frame':
            return f, tx
        if event == 'resync':
            await ws.send_bytes(W.pack(5, tx, b'\x01'))
            tx += 1
    raise AssertionError('No full snapshot')

async def main():
    report = {'sessions': [], 'health': []}
    timeout = aiohttp.ClientTimeout(total=None, connect=5)
    async with aiohttp.ClientSession(timeout=timeout) as client:
        async def metrics():
            async with client.get(args.health) as r:
                return await r.json()
        report['health'].append(await metrics())
        for i in range(args.sessions):
            started = time.monotonic()
            async with client.ws_connect(args.url, max_msg_size=W.MAX_PAYLOAD+16) as ws:
                await ws.send_bytes(W.pack(1, 0, W.hello(False)))
                f, tx = await screen(ws)
                assert (f.width, f.height) == (240, 135) and not f.granted
                assert f.minor == 0 and not f.keyboard
                report['sessions'].append(round(time.monotonic()-started, 3))
            await asyncio.sleep(0.08)
            if (i+1) % 10 == 0:
                report['health'].append(await metrics())
                print(f'{i+1}/{args.sessions} complete reconnect snapshots', flush=True)
        # Server must reject an input packet when HELLO requested view-only.
        async with client.ws_connect(args.url) as ws:
            await ws.send_bytes(W.pack(1, 0, W.hello(False)))
            _, tx = await screen(ws)
            await ws.send_bytes(W.pack(6, tx, W.POINTER.pack(120, 104, 1, 1)))
            while True:
                msg = await ws.receive(timeout=5)
                if msg.type != aiohttp.WSMsgType.BINARY:
                    assert msg.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED)
                    break
        await asyncio.sleep(0.1)
        # A keyboard-capable handshake still cannot bypass the control grant.
        async with client.ws_connect(args.url) as ws:
            await ws.send_bytes(W.pack(1, 0, W.hello(False, keyboard=True)))
            f = Framebuffer()
            while True:
                msg = await ws.receive(timeout=5)
                assert msg.type == aiohttp.WSMsgType.BINARY
                event, _ = f.receive(msg.data)
                if event == 'frame':
                    break
            assert f.minor == 1 and f.keyboard and not f.granted
            await ws.send_bytes(W.pack(7, 1, W.KEY.pack(ord('9'), True), minor=1))
            while True:
                msg = await ws.receive(timeout=5)
                if msg.type != aiohttp.WSMsgType.BINARY:
                    assert msg.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED)
                    break
        await asyncio.sleep(0.1)
        # Oversized claimed length is rejected before application parsing/copying.
        async with client.ws_connect(args.url) as ws:
            await ws.send_bytes(bytes.fromhex('525644500100010000000000ffffffff'))
            msg = await ws.receive(timeout=5)
            assert msg.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED)
        await asyncio.sleep(0.1)
        if args.seconds:
            async with client.ws_connect(args.url, max_msg_size=W.MAX_PAYLOAD+16) as ws:
                await ws.send_bytes(W.pack(1, 0, W.hello(False)))
                f, tx = await screen(ws)
                until = time.monotonic() + args.seconds
                last_ping = time.monotonic()
                batches = 0
                while time.monotonic() < until:
                    msg = await ws.receive(timeout=5)
                    assert msg.type == aiohttp.WSMsgType.BINARY, msg.type
                    event, _ = f.receive(msg.data)
                    if event == 'frame':
                        batches += 1
                    elif event == 'resync':
                        await ws.send_bytes(W.pack(5, tx, b'\x01')); tx += 1
                    if time.monotonic() - last_ping > 4:
                        await ws.send_bytes(W.pack(8, tx, b'\0\0\0\1'));tx += 1;last_ping = time.monotonic()
                report['soak_batches'] = batches
                report['soak_seconds'] = args.seconds
        report['health'].append(await metrics())
        assert report['health'][-1]['heap'] >= report['health'][0]['heap'] - 12000, 'Unexpected persistent heap loss'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2))
    print(f'PASS: {args.sessions} reconnects, unauthorized input/malformed packet rejected; report {args.output}', flush=True)

asyncio.run(main())
