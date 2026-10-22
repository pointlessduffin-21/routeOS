# RVDP 1.0 / 1.1 — lab profile

Status: implemented prototype, 2026-10-22. This replaces the conflicting RHMI
proposal. TCP/WebSocket packets are ordered within a connection only.

## Envelope

One **binary** WebSocket message per packet. Big endian for every multibyte
integer **including RGB565 pixels**. HTTP `/hmi`; WebSocket `:81/rvdp` on the
device. The host simulator serves both on the same port (default 8095).

| Offset | Field |
|---|---|
| 0 | ASCII `RVDP` (4 bytes) |
| 4 | major u8 = 1 |
| 5 | minor u8 = negotiated 0 or 1 |
| 6 | type u8 |
| 7 | flags u8 = 0 |
| 8 | per-direction sequence u32 |
| 12 | payload length u32 |

Maximum pixel bytes per rectangle: 16,384. Maximum payload: 16,401. Maximum
packet: 16,417. Validate message size before parsing. Reject text messages,
unknown types, flags, versions and length mismatches. No fragmented application
messages in the firmware profile. Each direction starts at sequence 0 and
increments exactly once per packet, modulo 2^32; reset on every connection.

## Payloads (fields in order, no padding)

| Type | Name | Direction | Payload |
|---|---|---|---|
| 1 | HELLO | client→device | min major/minor u8,u8; max major/minor u8,u8 (minimum 1.0; maximum 1.0 or 1.1); encoding mask u32=1; max pixel bytes u32 (2..16384); request control u8 (0/1) |
| 2 | DISPLAY_INFO | device→client | width,height u16,u16; pixel format u8=1; rotation u8=0; session ID u32; features u32 (bit0=pointer capability, bit1=keyboard capability in 1.1) |
| 3 | RECT | device→client | frame ID u32; x,y,w,h u16×4; encoding u8=1; pixel byte length u32; pixels |
| 4 | FRAME_END | device→client | frame ID u32; rectangle count u16 (1..512); full snapshot u8 (0/1); device uptime milliseconds u32 |
| 5 | RESYNC | client→device | reason u8 (0=initial, 1=overflow/incomplete, 2=user refresh) |
| 6 | POINTER | client→device | x,y u16,u16; buttons u8 (0/1); event u8 (0=move,1=down,2=up,3=cancel) |
| 7 | KEY | client to device | keysym u32; down u8 (0/1); negotiated 1.1 only |
| 8/9 | PING/PONG | either | nonce u32 |
| 10 | ERROR | either | code u16; UTF-8 message (maximum 128 bytes) |
| 11 | CONTROL_STATE | device→client | granted u8 (0/1) |

HELLO always uses envelope 1.0. The server chooses the highest supported minor
in the offered range and uses it in DISPLAY_INFO and every subsequent envelope.
Clients use that selected minor after DISPLAY_INFO. A 1.0 offer keeps the original
packet layout and pointer-only behavior; the simulator currently selects 1.0.
KEY and keyboard feature bit1 are forbidden in 1.0. Unknown feature bits are rejected.

KEY accepts printable ASCII 32..126 and X11 keysyms Backspace (0xff08), Tab
(0xff09), Return (0xff0d), Escape (0xff1b), Home (0xff50), Left (0xff51), Right
(0xff53), PageUp (0xff55), PageDown (0xff56), End (0xff57), Delete (0xffff).
The character already reflects Shift; modifiers and Unicode are not transported.
Actions happen on key-down; releases are accepted but do not repeat actions.
Both keyboard capability and CONTROL_STATE grant are required. The firmware's
32-event key queue disconnects on overflow rather than silently losing text.
Connection input epochs discard stale input; a visual RESYNC preserves typing.

No clipboard/audio/file transfer.
Dimensions are bounded to 320×240; firmware is fixed at 240×135. Every rectangle
must be nonempty, in bounds, encoding 1 and exactly `w*h*2` bytes, not exceeding
the negotiated limit. The current firmware profile requires HELLO max pixel
bytes of at least one draw strip (7,680 bytes); smaller offers are rejected.
The simulator supports smaller offers down to one row. Unsupported directions or payloads
close the session. Control capability and grant are distinct; viewers send no
pointer or keyboard packets until grant arrives, and the device validates permission too.

## Transactions and recovery

After HELLO the server sends DISPLAY_INFO and CONTROL_STATE, then a full repaint.
The client may request RESYNC. Frame IDs increment per LVGL render batch. Stage
RECTs without displaying; FRAME_END must match the ID and rectangle count. The
first accepted batch and every RESYNC require `full=1` and actual coverage of
every pixel. Never interpret `full=1` alone as proof of coverage. A new frame
before the previous FRAME_END discards the incomplete batch and requests resync.
Duplicate/stale frame IDs are rejected (modulo-32 ordering). Present only complete
batches. The ESP32 has no full framebuffer; full repaint is LVGL invalidation.

Firmware uses a bounded queue of ten owned tile packets (about 77 KiB) so an
initial nine-strip full repaint fits even before the sender runs. This is a
deliberate increase from the plan's provisional 2–4 slots: otherwise a synchronous
LVGL full render can overflow before a complete snapshot ever reaches the sender.
The queue is statically bounded, not a retained framebuffer. On overflow, flag
the batch damaged, omit FRAME_END, drain, and schedule a fresh full invalidation.
After disconnect, release input and clear old queued packets before a new HELLO.
No frame generation may cross a session boundary.

One viewer only; reject a second connection. Input is opt-in at compile time and
must also be requested by HELLO. Release on disconnect, cancel, or 2-second stale
press; bounded input queue overflow releases the pointer rather than losing an up.
PING/PONG provides liveness; clients time out and offer reconnect. No assumption
that inactivity implies screen damage.

## Security and golden vectors

This is an isolated-LAN prototype. Device AP uses a fresh random WPA2 password;
network membership is not production authorization. Use an authenticated TLS
gateway or add authenticated WSS before deployment. No device keys or access
tokens are stored in the connection library. Plain `ws://` is lab-only; the native
client's `wss://` transport retains certificate verification.

Golden vectors in `vectors/v1.json` are hexadecimal packets independent of
runtime encoder output. `rvdp.py` and iShareScreen `protocols/rvdp/wire.py` must
remain byte-identical. Tests cover malformed lengths, bounds, byte order,
sequence/session reset, incomplete full snapshots and live fake-server behavior.
