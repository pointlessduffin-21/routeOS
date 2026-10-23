#include "rvdp_server.h"

#include <atomic>
#include <errno.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/jpeg_encode.h>
#include <lwip/sockets.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
// LVGL has no getter for the flush callback, and the adapter installs its own, so the old one is read from the struct
// and called after the capture: replacing it outright would stop the panel from being drawn.
#include "src/display/lv_display_private.h"

#include "p4_transport.h"
#include "routeos.h"
#include "rvdp_protocol.h"

namespace rvdp_transport {
namespace {

constexpr char TAG[] = "rvdp";

constexpr uint16_t W = rvdp::WIDTH;
constexpr uint16_t H = rvdp::HEIGHT;
constexpr uint16_t ROWS = rvdp::STRIP_ROWS;         // one packet per band
constexpr int BANDS = (H + ROWS - 1) / ROWS;
constexpr size_t BAND_BYTES = size_t(W) * ROWS * 2;
constexpr size_t PACKET_MAX = 16 + 17 + BAND_BYTES;

constexpr uint32_t HELLO_WAIT_MS = 5000;            // a client that never says hello
constexpr uint32_t IDLE_MS = 20000;                 // no packet from the viewer
constexpr uint32_t STALL_MS = 20000;                // socket room, but nothing getting through
constexpr uint32_t CONTROL = 1;                     // this build grants keyboard and pointer

constexpr const char BUSY[] = "routeOS display is busy. Close the other browser tab or iShareScreen session, then retry.";

uint16_t* shadow;                                   // PSRAM, W*H: the last frame drawn
uint8_t* packet;                                    // PSRAM, PACKET_MAX: one outgoing frame at a time
uint8_t dirty[BANDS];                               // changed since a batch was taken
int batch[BANDS];                                   // bands in the batch being sent
int batchTotal, batchSent, rectsSent;
uint32_t batchId, frameId;
bool batchActive, batchFull;

lv_display_t* display;
lv_display_flush_cb_t adapterFlush;

// One band as JPEG. A flat UI band compresses about ten to one, and on a weak link the air time matters far more
// than the CPU time: the ESP32-P4's JPEG peripheral encodes 800x8 in well under a millisecond.
jpeg_encoder_handle_t encoder;
uint8_t* jpegIn;                                    // one band, RGB565 little endian
uint8_t* jpegOut;
uint32_t jpegOutCapacity;
bool peerJpeg;                                      // the viewer said it can decode encoding 2
constexpr uint32_t JPEG_QUALITY = 50;
// Consecutive dirty bands travel as one JPEG: each image repeats its own tables (~600 B), and on a slow link every
// extra round trip costs far more than the bytes saved.
constexpr uint16_t JPEG_GROUP = 8;
constexpr size_t JPEG_MAX_BYTES = BAND_BYTES * JPEG_GROUP;

httpd_handle_t server;
std::atomic<int> viewer{-1};                        // the socket that owns the display, or -1
bool helloDone, granted;
uint8_t peerMinor;
uint32_t txSeq, rxSeq, connectedAt, lastRx, lastSent;
SemaphoreHandle_t wake, sendLock;
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> frameVersion{0};     // changes when the panel frame does

rvdp::Input pointer{0, 0, false, 0};
uint32_t pointerAt;

uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

bool socketWritable(int fd) {
    fd_set writable;
    struct timeval none = {0, 0};
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    return select(fd + 1, nullptr, &writable, nullptr, &none) > 0 && FD_ISSET(fd, &writable);
}

void closeSocket(int fd) {
    if (server && fd >= 0) httpd_sess_trigger_close(server, fd);
}

// ---- Capture ----
// Runs in the adapter's LVGL task, from the flush callback, so the pixels are copied before the adapter hands the
// buffer on (it may be reused as soon as flush_ready is called).
void capture(const lv_area_t* area, const uint8_t* pixels) {
    if (!shadow || !pixels) return;
    const int x1 = area->x1 < 0 ? 0 : area->x1;
    const int y1 = area->y1 < 0 ? 0 : area->y1;
    const int x2 = area->x2 >= W ? W - 1 : area->x2;
    const int y2 = area->y2 >= H ? H - 1 : area->y2;
    if (x2 < x1 || y2 < y1) return;
    const size_t srcStride = size_t(area->x2 - area->x1 + 1) * 2;
    const size_t copy = size_t(x2 - x1 + 1) * 2;
    const uint8_t* src = pixels + size_t(y1 - area->y1) * srcStride + size_t(x1 - area->x1) * 2;
    uint8_t* dst = reinterpret_cast<uint8_t*>(shadow) + (size_t(y1) * W + size_t(x1)) * 2;
    for (int y = y1; y <= y2; ++y) {
        memcpy(dst, src, copy);
        dst += size_t(W) * 2;
        src += srcStride;
    }
    portENTER_CRITICAL(&stateLock);
    for (int y = y1; y <= y2; ++y) dirty[y / ROWS] = 1;
    portEXIT_CRITICAL(&stateLock);
    frameVersion.fetch_add(1);
    // Nobody is watching: do not wake the sender at all, so an idle board costs nothing per frame drawn.
    if (viewer.load() >= 0) xSemaphoreGive(wake);
}

void flush(lv_display_t* disp, const lv_area_t* area, uint8_t* pixels) {
    capture(area, pixels);
    if (adapterFlush) adapterFlush(disp, area, pixels);
}

// ---- Sending ----
// Every outgoing frame uses `packet`, so the payload, the sequence number and the hand-off to the socket all happen
// under one lock: two tasks allocating sequence numbers out of order would make the viewer drop the session.
bool sendLocked(int fd, uint8_t type, size_t payload) {
    rvdp::header(packet, type, txSeq++, payload, peerMinor);
    const size_t total = payload + 16;
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = packet;
    frame.len = total;
    // The socket belongs to httpd: writing to it from this task would fight httpd for it, so every frame goes through
    // the supported API. A compressed block is a few kilobytes, which fits the TCP send buffer, so a frame is written
    // in one go instead of being abandoned half way (which is what used to desynchronise the stream).
    const esp_err_t err = httpd_ws_send_frame_async(server, fd, &frame);
    if (err != ESP_OK) {
        static int reported = 0;
        if (reported < 4) {
            reported++;
            routeos::log('W', "rvdp: send of %u bytes failed: %s", (unsigned)total, esp_err_to_name(err));
        }
        return false;
    }
    hmi::sentBytes += total;
    return true;
}

bool sendSmall(int fd, uint8_t type, const uint8_t* payload, size_t length) {
    if (length > PACKET_MAX - 16) return false;
    xSemaphoreTake(sendLock, portMAX_DELAY);
    memcpy(packet + 16, payload, length);
    const bool ok = sendLocked(fd, type, length);
    xSemaphoreGive(sendLock);
    return ok;
}

// ---- JPEG bands ----
void initEncoder() {
    if (encoder) return;
    jpeg_encode_engine_cfg_t engine = {};
    engine.timeout_ms = 200;
    if (jpeg_new_encoder_engine(&engine, &encoder) != ESP_OK) {
        encoder = nullptr;
        ESP_LOGW(TAG, "no JPEG encoder available, bands stay raw");
        return;
    }
    jpeg_encode_memory_alloc_cfg_t in_cfg = {};
    in_cfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
    jpeg_encode_memory_alloc_cfg_t out_cfg = {};
    out_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
    size_t in_size = 0, out_size = 0;
    jpegIn = (uint8_t*)jpeg_alloc_encoder_mem(JPEG_MAX_BYTES, &in_cfg, &in_size);
    jpegOut = (uint8_t*)jpeg_alloc_encoder_mem(JPEG_MAX_BYTES, &out_cfg, &out_size);
    jpegOutCapacity = uint32_t(out_size);
    if (!jpegIn || !jpegOut) {
        ESP_LOGW(TAG, "no memory for the JPEG band buffers, bands stay raw");
        jpeg_del_encoder_engine(encoder);
        encoder = nullptr;
        jpegIn = nullptr;
        jpegOut = nullptr;
        return;
    }
    ESP_LOGI(TAG, "JPEG bands ready: quality %u, %u B in, %u B out", (unsigned)JPEG_QUALITY, (unsigned)in_size,
             (unsigned)out_size);
}

// Encodes `rows` lines from the shadow into the space after the 17-byte RECT header. Returns false when the result
// would not be smaller than the raw pixels, so the caller can always fall back to encoding 1.
bool encodeBand(uint16_t y, uint16_t rows, size_t max_raw, size_t* size) {
    if (!encoder || !jpegIn || !jpegOut) return false;
    const uint16_t* src = shadow + size_t(y) * W;
    uint16_t* dst = (uint16_t*)jpegIn;
    for (uint16_t row = 0; row < rows; ++row) {
        memcpy(dst + size_t(row) * W, src + size_t(row) * W, size_t(W) * 2);
    }
    jpeg_encode_cfg_t cfg = {};
    cfg.height = rows;
    cfg.width = W;
    cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
    cfg.image_quality = JPEG_QUALITY;
    uint32_t out = 0;
    if (jpeg_encoder_process(encoder, &cfg, jpegIn, uint32_t(rows) * W * 2, jpegOut, jpegOutCapacity, &out) != ESP_OK) {
        return false;
    }
    if (!out || out > max_raw || out > PACKET_MAX - 16 - 17) return false;   // never overflow the packet buffer
    memcpy(packet + 16 + 17, jpegOut, out);
    *size = out;
    return true;
}

// Sends the dirty run that starts at `y`: one JPEG when the viewer accepts them, otherwise one raw band. `covered`
// reports how many bands the frame accounted for, so the caller can advance past them.
bool sendBands(int fd, uint16_t y, uint16_t rows, uint32_t id, int* covered) {
    xSemaphoreTake(sendLock, portMAX_DELAY);
    uint8_t* p = packet + 16;
    uint16_t sendRows = rows;
    int sendBands = (rows + ROWS - 1) / ROWS;
    uint8_t encoding = 1;
    size_t length = size_t(W) * rows * 2;
    size_t jpeg_size = 0;
    if (!peerJpeg || !encodeBand(y, rows, length, &jpeg_size)) {
        // Raw fallback is always a single band: a whole group of raw pixels would not fit the packet buffer.
        sendRows = (y + ROWS <= H) ? ROWS : (H - y);
        sendBands = 1;
        encoding = 1;
        length = size_t(W) * sendRows * 2;
        const uint16_t* line = shadow + size_t(y) * W;
        for (size_t i = 0; i < size_t(W) * sendRows; ++i) rvdp::put16(p + 17 + i * 2, line[i]);
    } else {
        encoding = 2;
        length = jpeg_size;
    }
    rvdp::put32(p, id);
    rvdp::put16(p + 4, 0);
    rvdp::put16(p + 6, y);
    rvdp::put16(p + 8, W);
    rvdp::put16(p + 10, sendRows);
    p[12] = encoding;
    rvdp::put32(p + 13, (uint32_t)length);
    const bool ok = sendLocked(fd, rvdp::RECT, 17 + length);
    xSemaphoreGive(sendLock);
    static int logged = 0;
    if (logged < 8) {
        logged++;
        routeos::log('I', "rvdp: y=%u rows=%u enc=%u len=%u covered=%d ok=%d", y, sendRows, encoding,
                     (unsigned)length, sendBands, (int)ok);
    }
    *covered = sendBands;
    return ok;
}

bool sendEnd(int fd, uint32_t id, uint16_t count, bool full) {
    uint8_t payload[11];
    rvdp::put32(payload, id);
    rvdp::put16(payload + 4, count);
    payload[6] = full ? 1 : 0;
    rvdp::put32(payload + 7, nowMs());
    return sendSmall(fd, rvdp::FRAME_END, payload, sizeof(payload));
}

// ---- Viewer bookkeeping ----
void resetViewer() {
    viewer = -1;
    helloDone = granted = false;
    peerMinor = 0;
    peerJpeg = false;
    txSeq = rxSeq = 0;
    batchActive = false;
    batchSent = batchTotal = 0;
    portENTER_CRITICAL(&stateLock);
    memset(dirty, 0, sizeof(dirty));
    portEXIT_CRITICAL(&stateLock);
    hmi::inputReady = false;
    hmi::inputEpoch++;
    hmi::epoch++;
    xQueueReset(hmi::keys);
    xQueueReset(hmi::inputs);
}

void dropViewer(int fd) {
    if (viewer == fd) resetViewer();
    routeos::log('W', "rvdp: dropping viewer on socket %d (%u bands sent)", fd, (unsigned)batchSent);
    closeSocket(fd);
}

void adoptViewer(int fd) {
    resetViewer();
    viewer = fd;
    connectedAt = lastRx = lastSent = nowMs();
    xSemaphoreGive(wake);
}

void requestFull() {
    // A snapshot supersedes whatever batch was in flight: half a frame followed by a new one would fail the viewer's
    // coverage check and bounce it into a resync loop.
    portENTER_CRITICAL(&stateLock);
    memset(dirty, 1, sizeof(dirty));
    portEXIT_CRITICAL(&stateLock);
    batchActive = false;
    batchSent = batchTotal = 0;
    rectsSent = 0;
    frameId = 0;
    lastSent = nowMs();
    xSemaphoreGive(wake);
}

void guardViewer() {
    const int fd = viewer;
    if (fd < 0) return;
    const uint32_t now = nowMs();
    if ((!helloDone && now - connectedAt > HELLO_WAIT_MS) || now - lastRx > IDLE_MS ||
        (batchActive && now - lastSent > STALL_MS)) {
        routeos::log('W', "display viewer dropped: nothing from it or nothing getting through");
        dropViewer(fd);
    }
}

// ---- WebSocket ----
esp_err_t handler(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        // A WebSocket handshake. The viewer that is already watching keeps the display: two clients that reconnect on
        // their own would otherwise keep stealing it from each other, and every steal throws away a half-sent frame and
        // starts a full one, which is what makes the screen look slow and flaky. A viewer that has gone quiet is
        // replaced, because lwIP recycles descriptor numbers and a half-open socket would keep the port silent.
        if (viewer != fd) {
            // Each rectangle is written as a header and a payload in separate small writes. With Nagle on, the second
            // write waits for an ACK, and the peer holds that ACK for its delayed-ACK timer: about 200 ms per rectangle,
            // which is what made a full screen take seconds on a link that was otherwise fine.
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            const int previous = viewer;
            const uint32_t now = nowMs();
            const bool gone = previous >= 0 && httpd_ws_get_fd_info(server, previous) == HTTPD_WS_CLIENT_INVALID;
            const bool stale = previous < 0 || gone || (!helloDone && now - connectedAt > HELLO_WAIT_MS) ||
                               now - lastRx > IDLE_MS;
            if (stale) {
                adoptViewer(fd);
                if (previous >= 0) closeSocket(previous);
            } else {
                routeos::log('W', "rvdp: second viewer on socket %d, keeping the one already watching", fd);
            }
        }
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_BINARY;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        // The socket is gone. Free the display now rather than waiting out the idle timeout, or the next viewer is
        // turned away from a screen nobody is watching.
        if (viewer == fd) dropViewer(fd);
        return err;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        routeos::log('W', "rvdp in: close frame from viewer");
        dropViewer(fd);
        return ESP_OK;
    }
    if (frame.type == HTTPD_WS_TYPE_PING) return ESP_OK;     // the server answers pings itself
    if (frame.type != HTTPD_WS_TYPE_BINARY || frame.len < 16 || frame.len > 1024) {
        routeos::log('W', "rvdp in: bad frame type=%d len=%u", (int)frame.type, (unsigned)frame.len);
        dropViewer(fd);
        return ESP_OK;
    }
    uint8_t buf[1024];
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        if (viewer == fd) dropViewer(fd);
        return err;
    }

    uint8_t type;
    uint32_t sequence;
    const uint8_t* p;
    size_t n;
    if (!rvdp::decode(buf, frame.len, type, sequence, p, n)) {
        dropViewer(fd);
        return ESP_OK;
    }
    if (viewer != fd) {
        // A frame from a socket we do not own: tell it the display is taken, on its own socket.
        // The buffer has to outlive this call: the asynchronous send is handed to the server task, so a stack buffer
        // would be overwritten before the bytes leave. One static buffer is enough - only the server task writes here.
        static uint8_t rejection[16 + 2 + sizeof(BUSY) - 1];
        rvdp::header(rejection, rvdp::ERROR, 0, 2 + sizeof(BUSY) - 1);
        rvdp::put16(rejection + 16, 409);
        memcpy(rejection + 18, BUSY, sizeof(BUSY) - 1);
        routeos::log('W', "display viewer turned away: another viewer is connected");
        xSemaphoreTake(sendLock, portMAX_DELAY);
        httpd_ws_frame_t busy_frame = {};
        busy_frame.type = HTTPD_WS_TYPE_BINARY;
        busy_frame.payload = rejection;
        busy_frame.len = sizeof(rejection);
        const esp_err_t sent = httpd_ws_send_frame_async(server, fd, &busy_frame);
        xSemaphoreGive(sendLock);
        routeos::log('W', "rvdp: busy reply to socket %d: %s (closing)", fd, esp_err_to_name(sent));
        // This board's stack drops the queued frame when the socket closes right after it, so the message usually does
        // not reach the viewer - it just sees a closed connection. Closing is still the honest choice: the socket is
        // not the owner, and leaving it open would hold one of only three slots. Viewers that care read "closed before
        // any packet" as "someone else is watching" (Termz does exactly that).
        closeSocket(fd);
        return ESP_OK;
    }
    if (sequence != rxSeq) {
        dropViewer(fd);
        return ESP_OK;
    }
    rxSeq++;
    lastRx = nowMs();

    if (!helloDone) {
        // Bit 0 of the encodings field is raw RGB565 (mandatory), bit 1 is JPEG rectangles.
        const uint32_t encodings = rvdp::u32(p + 4);
        if (type != rvdp::HELLO || n != 13 || p[0] != 1 || p[1] || p[2] != 1 || p[3] > 1 ||
            (encodings & 1) == 0 || (encodings & ~uint32_t(3)) != 0 ||
            rvdp::u32(p + 8) < rvdp::TILE_BYTES || rvdp::u32(p + 8) > 16384 || p[12] > 1) {
            dropViewer(fd);
            return ESP_OK;
        }
        helloDone = true;
        peerMinor = p[3];
        peerJpeg = (encodings & 2) && encoder != nullptr;
        granted = CONTROL && p[12];
        hmi::inputEpoch++;
        hmi::inputReady = granted;
        uint8_t info[14];
        rvdp::put16(info, W);
        rvdp::put16(info + 2, H);
        info[4] = 1;
        info[5] = 0;
        rvdp::put32(info + 6, hmi::epoch.load());
        rvdp::put32(info + 10, CONTROL ? (peerMinor ? 3 : 1) : 0);
        sendSmall(fd, rvdp::DISPLAY_INFO, info, sizeof(info));
        uint8_t permission = granted ? 1 : 0;
        sendSmall(fd, rvdp::CONTROL_STATE, &permission, 1);
        routeos::log('I', "display viewer connected (%ux%u, %scontrol)", W, H, granted ? "" : "no ");
        requestFull();
        return ESP_OK;
    }

    if (type == rvdp::RESYNC && n == 1 && p[0] <= 2) {
        requestFull();
        return ESP_OK;
    }
    if (type == rvdp::PING && n == 4) {
        sendSmall(fd, rvdp::PONG, p, 4);
        return ESP_OK;
    }
    if (type == rvdp::PONG && n == 4) return ESP_OK;
    if (buf[5] != peerMinor) {
        dropViewer(fd);
        return ESP_OK;
    }
    if (type == rvdp::KEY && peerMinor == 1 && granted && n == 5) {
        const uint32_t key = rvdp::u32(p);
        if (!rvdp::validKey(key) || p[4] > 1) {
            dropViewer(fd);
            return ESP_OK;
        }
        rvdp::KeyInput input{key, p[4] != 0, hmi::inputEpoch.load()};
        if (xQueueSend(hmi::keys, &input, 0) != pdTRUE) {
            xQueueReset(hmi::keys);
            hmi::drops++;
            dropViewer(fd);
        }
        return ESP_OK;
    }
    if (type == rvdp::POINTER && granted && n == 6) {
        const uint16_t x = rvdp::u16(p), y = rvdp::u16(p + 2);
        if (x >= W || y >= H || p[4] > 1 || p[5] > 3 || (p[5] >= 2 && p[4])) {
            dropViewer(fd);
            return ESP_OK;
        }
        rvdp::Input input{x, y, p[4] != 0, hmi::inputEpoch.load()};
        if (xQueueSend(hmi::inputs, &input, 0) != pdTRUE) {
            xQueueReset(hmi::inputs);
            input.down = false;
            xQueueSend(hmi::inputs, &input, 0);
            hmi::drops++;
        }
        return ESP_OK;
    }
    dropViewer(fd);
    return ESP_OK;
}

void senderTask(void*) {
    for (;;) {
        // With no viewer there is nothing to do, so block until one arrives instead of polling forever.
        if (viewer.load() < 0) {
            xSemaphoreTake(wake, portMAX_DELAY);
            continue;
        }
        // Waiting a quarter second for room in a full socket turns a 2 MB snapshot into a minute of trickle. While a
        // batch is in flight, come back as soon as the socket drains; when idle, sleep until there is something to do.
        xSemaphoreTake(wake, pdMS_TO_TICKS(batchActive ? 10 : 250));
        static uint32_t lastBeat = 0;
        const uint32_t beatNow = nowMs();
        if (beatNow - lastBeat > 5000) {
            lastBeat = beatNow;
            routeos::log('I', "rvdp: beat fd=%d hello=%d active=%d sent=%d/%d idle=%ums pending=%u", viewer.load(),
                         (int)helloDone, (int)batchActive, batchSent, batchTotal,
                         (unsigned)(beatNow - lastSent), (unsigned)pending());
        }
        const int fd = viewer.load();
        if (fd < 0 || !helloDone) {
            guardViewer();
            continue;
        }
        if (!batchActive) {
            portENTER_CRITICAL(&stateLock);
            int count = 0;
            bool all = true;
            for (int band = 0; band < BANDS; ++band) {
                if (dirty[band]) {
                    dirty[band] = 0;
                    batch[count++] = band;
                } else {
                    all = false;
                }
            }
            portEXIT_CRITICAL(&stateLock);
            if (!count) {
                guardViewer();
                continue;
            }
            batchTotal = count;
            batchSent = 0;
            rectsSent = 0;
            batchFull = all;
            batchId = ++frameId;
            batchActive = true;
        }
        while (batchSent < batchTotal) {
            const int band = batch[batchSent];
            const uint16_t y = uint16_t(band) * ROWS;
            // Take the run of consecutive dirty bands, up to a group, so one JPEG covers as many of them as possible.
            int group = 1;
            if (peerJpeg) {
                while (group < JPEG_GROUP && batchSent + group < batchTotal && batch[batchSent + group] == band + group) {
                    group++;
                }
            }
            uint16_t rows = uint16_t(group) * ROWS;
            if (y + rows > H) rows = H - y;
            // A viewer that stops reading must not hold the only send slot: give up on it and free the port.
            if (nowMs() - lastSent > STALL_MS) {
                dropViewer(fd);
                break;
            }
            if (!socketWritable(fd)) {
                static int refused = 0;
                if (refused < 6) {
                    refused++;
                    routeos::log('W', "rvdp: socket %d not writable yet (batch %d/%d)", fd, batchSent, batchTotal);
                }
                break;      // the queue keeps the batch; a slow link must not stop HTTP
            }
            int covered = 1;
            if (!sendBands(fd, y, rows, batchId, &covered)) {
                dropViewer(fd);
                break;
            }
            batchSent += covered;
            rectsSent++;
            lastSent = nowMs();
        }
        if (batchActive && batchSent >= batchTotal) {
            // The count is rectangles, not bands: a compressed rectangle can cover several bands.
            sendEnd(fd, batchId, uint16_t(rectsSent), batchFull);
            hmi::batches++;
            batchActive = false;
            lastSent = nowMs();
        }
        guardViewer();
    }
}

// Runs outside senderTask so a send that blocks inside httpd cannot leave the display transport wedged: if bands are
// waiting and nothing has moved for a while, the viewer is dropped and the port goes back to idle, ready for the next
// connection instead of staying silent.
void watchdogTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const int fd = viewer.load();
        if (fd < 0 || !helloDone) continue;
        const uint32_t stalled = nowMs() - lastSent;
        if (pending() > 0 && stalled > STALL_MS) {
            routeos::log('W', "rvdp: stalled for %ums with %u bands waiting, resetting the viewer", (unsigned)stalled,
                         (unsigned)pending());
            dropViewer(fd);
        }
    }
}

void readInput(lv_indev_t*, lv_indev_data_t* data) {
    rvdp::Input next;
    // Keep down/up transitions: a fast click must not collapse into "never pressed".
    if (xQueueReceive(hmi::inputs, &next, 0) == pdTRUE) {
        pointer = next;
        pointerAt = nowMs();
    }
    if (pointer.epoch != hmi::inputEpoch.load() || !hmi::inputReady || nowMs() - pointerAt > 2000) pointer.down = false;
    data->point.x = pointer.x;
    data->point.y = pointer.y;
    data->state = pointer.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = uxQueueMessagesWaiting(hmi::inputs) > 0;
}

}  // namespace

void attach(lv_display_t* disp) {
    if (!disp || shadow) return;
    shadow = (uint16_t*)heap_caps_malloc(size_t(W) * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    packet = (uint8_t*)heap_caps_malloc(PACKET_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!shadow || !packet) {
        ESP_LOGE(TAG, "no PSRAM for the %ux%u screen shadow", W, H);
        heap_caps_free(shadow);
        heap_caps_free(packet);
        shadow = nullptr;
        packet = nullptr;
        return;
    }
    memset(shadow, 0, size_t(W) * H * 2);
    initEncoder();
    wake = xSemaphoreCreateBinary();
    sendLock = xSemaphoreCreateMutex();
    display = disp;
    adapterFlush = disp->flush_cb;
    lv_display_set_flush_cb(disp, flush);
    // A second pointer device, alongside the panel's own touch controller: clicks that arrive over RVDP.
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, readInput);
    lv_indev_set_display(indev, disp);
    ESP_LOGI(TAG, "shadow %ux%u in PSRAM, %d bands of %u rows", W, H, BANDS, ROWS);
}

void begin() {
    if (!shadow) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 81;
    config.ctrl_port = 32081;             // the management server on 80 keeps the default
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    // A viewer that dies mid-frame would otherwise sit in a blocking send for the default five seconds per band.
    // The stall check in senderTask is what finally frees the slot; this only bounds each individual write.
    config.send_wait_timeout = 5;
    // httpd closes a socket that has sent nothing for recv_wait_timeout seconds. A viewer that is only watching is
    // idle for minutes at a time, so this must not be the default five.
    config.recv_wait_timeout = 30;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "cannot start the display server on port 81");
        server = nullptr;
        return;
    }
    httpd_uri_t uri = {};
    uri.uri = "/rvdp";
    uri.method = HTTP_GET;
    uri.handler = handler;
    uri.is_websocket = true;
    uri.handle_ws_control_frames = true;
    httpd_register_uri_handler(server, &uri);
    xTaskCreate(senderTask, "rvdp", 6144, nullptr, 3, nullptr);
    xTaskCreate(watchdogTask, "rvdp-wd", 3072, nullptr, 2, nullptr);
    routeos::log('I', "display: rvdp on ws://:81/rvdp, %ux%u", W, H);
}

uint32_t pending() {
    uint32_t waiting = batchActive ? uint32_t(batchTotal - batchSent) : 0;
    portENTER_CRITICAL(&stateLock);
    for (int band = 0; band < BANDS; ++band) waiting += dirty[band] ? 1 : 0;
    portEXIT_CRITICAL(&stateLock);
    return waiting;
}

const void* panel_frame() { return shadow; }

uint32_t panel_frame_version() { return frameVersion.load(); }

}  // namespace rvdp_transport
