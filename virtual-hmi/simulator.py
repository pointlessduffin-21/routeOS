"""Deterministic RVDP device for browser/native integration; no physical hardware.

Run with iShareScreen's Python (aiohttp already installed) or requirements-host.txt.
Defaults to loopback. --control explicitly allows the demo button's pointer input.
"""
from __future__ import annotations
import argparse
import asyncio
from pathlib import Path
import struct
import sys
import time
from aiohttp import web, WSMsgType

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol import rvdp as W

WIDTH, HEIGHT = 240, 135
WEB = Path(__file__).parent / 'web'


def create_app(control=False):
    app = web.Application(client_max_size=1024)
    app['active'] = False
    app['clicks'] = 0
    app['sessions'] = 0

    async def viewer(req):
        html = (WEB / 'index.html').read_text(encoding='utf-8')
        for name in ('wire.js', 'viewer.js'):
            html = html.replace(f'<script src="/{name}"></script>', '<script>' + (WEB / name).read_text(encoding='utf-8') + '</script>')
        return web.Response(text=html, content_type='text/html', charset='utf-8', headers={'Cache-Control': 'no-store'})

    async def javascript(req):
        return web.FileResponse(WEB / req.match_info['name'])

    async def health(req):
        return web.json_response({'simulator': True, 'width': WIDTH, 'height': HEIGHT,
                                  'clicks': app['clicks'], 'sessions': app['sessions']})

    async def socket(req):
        if app['active']:
            return web.Response(status=409, text='One viewer at a time')
        ws = web.WebSocketResponse(max_msg_size=1024, compress=False)
        await ws.prepare(req)
        app['active'] = True
        app['sessions'] += 1
        session_id = app['sessions']
        sequence = frame = 0
        incoming = W.Sequence()
        granted = False
        pressed = False
        start = time.monotonic()

        async def send(kind, payload):
            nonlocal sequence
            await ws.send_bytes(W.pack(kind, sequence, payload))
            sequence = (sequence + 1) & 0xffffffff

        async def repaint(maximum):
            nonlocal frame
            frame += 1
            count = 0
            rows = max(1, min(16, maximum // (WIDTH * 2)))
            for y in range(0, HEIGHT, rows):
                h = min(rows, HEIGHT - y)
                pixels = bytearray(WIDTH * h * 2)
                for row in range(h):
                    for x in range(WIDTH):
                        colour = (0xf800, 0x07e0, 0x001f)[x // 80] if y + row < 24 else 0x18e3
                        if 60 <= x < 180 and 75 <= y + row < 120:
                            colour = 0x07e0 if app['clicks'] % 2 else 0x041f
                        # A deterministic moving white marker represents the clock.
                        if 32 <= y + row < 48 and (frame * 8) % WIDTH <= x < (frame * 8) % WIDTH + 8:
                            colour = 0xffff
                        struct.pack_into('>H', pixels, (row * WIDTH + x) * 2, colour)
                await send(W.Type.RECT, W.rectangle(frame, 0, y, WIDTH, h, pixels))
                count += 1
            await send(W.Type.FRAME_END, W.END.pack(frame, count, 1, int((time.monotonic() - start) * 1000)))

        try:
            message = await asyncio.wait_for(ws.receive(), 5)
            if message.type != WSMsgType.BINARY:
                raise ValueError('Expected binary HELLO')
            packet = W.unpack(message.data)
            incoming.accept(packet.sequence)
            if packet.sequence != 0 or packet.kind != W.Type.HELLO:
                raise ValueError('HELLO must be first')
            maximum, requested = W.read_hello(packet.payload)
            if maximum < WIDTH * 2:
                raise ValueError('Rectangle limit cannot fit a row')
            granted = control and requested
            await send(W.Type.DISPLAY_INFO, W.display_info(WIDTH, HEIGHT, session_id, control))
            await send(W.Type.CONTROL_STATE, bytes([granted]))
            await repaint(maximum)
            while not ws.closed:
                try:
                    message = await ws.receive(timeout=1)
                except asyncio.TimeoutError:
                    pressed = False
                    await repaint(maximum)
                    continue
                if message.type in (WSMsgType.CLOSE, WSMsgType.CLOSED, WSMsgType.ERROR):
                    break
                if message.type != WSMsgType.BINARY:
                    raise ValueError('Expected binary packet')
                packet = W.unpack(message.data)
                incoming.accept(packet.sequence)
                if packet.kind == W.Type.RESYNC and len(packet.payload) == 1 and packet.payload[0] <= 2:
                    await repaint(maximum)
                elif packet.kind == W.Type.POINTER and granted:
                    x, y, down, event = W.read_pointer(packet.payload, WIDTH, HEIGHT)
                    inside = 60 <= x < 180 and 75 <= y < 120
                    if pressed and not down and inside:
                        app['clicks'] += 1
                        await repaint(maximum)
                    pressed = bool(down and inside)
                elif packet.kind == W.Type.PING and len(packet.payload) == 4:
                    await send(W.Type.PONG, packet.payload)
                elif packet.kind == W.Type.PONG and len(packet.payload) == 4:
                    pass
                else:
                    raise ValueError('Unsupported or unauthorized message')
        except (ValueError, asyncio.TimeoutError):
            await ws.close(code=1002, message=b'Invalid RVDP session')
        finally:
            app['active'] = False
        return ws

    app.router.add_get('/hmi', viewer)
    app.router.add_get('/{name:viewer.js|wire.js}', javascript)
    app.router.add_get('/health', health)
    app.router.add_get('/rvdp', socket)
    return app


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8095)
    parser.add_argument('--control', action='store_true')
    args = parser.parse_args()
    web.run_app(create_app(args.control), host=args.host, port=args.port)
