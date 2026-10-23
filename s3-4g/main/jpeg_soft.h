// Baseline JPEG encoder, small enough for a chip without a JPEG peripheral.
//
// The ESP32-S3 has to send the whole screen over a cellular link that moves about 6 KB/s, and a raw 480x800 RGB565 frame
// is 768 KB. This encoder turns a band of the screen into a JPEG the viewer can already decode, so RVDP needs no new
// encoding. 4:2:2 sampling is used because bands are eight rows tall (one MCU row) and chroma detail on a flat UI is
// worth nothing.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Encodes `rgb565` (little-endian RGB565, `width * height` pixels) into `out`.
// Returns the number of bytes written, or 0 when it does not fit or the input is not usable.
size_t jpeg_soft_encode(const uint16_t* rgb565, int width, int height, int quality, uint8_t* out, size_t capacity);

#ifdef __cplusplus
}
#endif
