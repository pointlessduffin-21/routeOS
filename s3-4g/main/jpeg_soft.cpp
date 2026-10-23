// Baseline (SOF0) JPEG encoder: 8-bit, three components, 4:2:2, standard Huffman tables, quality-scaled quantization.
//
// Written for the ESP32-S3, which has no JPEG peripheral. Correctness first: plain float DCT, standard tables, no
// restart markers. A 480x64 band costs a couple of milliseconds and comes out ten to twenty times smaller than raw
// RGB565, which is the difference between a 6 KB/s cellular link being usable and not.
#include "jpeg_soft.h"

#include <math.h>
#include <string.h>

namespace {

// Natural (row major) order, straight from the JPEG specification, Annex K.
const uint8_t kLumaQuant[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};

const uint8_t kChromaQuant[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

// zigzag[i] = index in natural order of the i-th coefficient in scan order.
const uint8_t kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// Canonical Huffman tables, Annex K.3.
const uint8_t kDcLumaBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const uint8_t kDcLumaValues[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint8_t kDcChromaBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const uint8_t kDcChromaValues[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

const uint8_t kAcLumaBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const uint8_t kAcLumaValues[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

const uint8_t kAcChromaBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
const uint8_t kAcChromaValues[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

struct HuffTable {
    uint16_t code[256];
    uint8_t size[256];
};

struct BitWriter {
    uint8_t* out;
    size_t capacity;
    size_t written;
    uint32_t buffer;
    int bits;
    bool overflow;
};

void putByte(BitWriter* w, uint8_t value) {
    if (w->written >= w->capacity) {
        w->overflow = true;
        return;
    }
    w->out[w->written++] = value;
}

void putBits(BitWriter* w, uint32_t value, int count) {
    if (count <= 0) return;
    w->buffer = (w->buffer << count) | (value & ((1u << count) - 1));
    w->bits += count;
    while (w->bits >= 8) {
        const uint8_t byte = uint8_t(w->buffer >> (w->bits - 8));
        putByte(w, byte);
        if (byte == 0xFF) putByte(w, 0x00);   // byte stuffing, mandatory inside entropy data
        w->bits -= 8;
    }
}

void flushBits(BitWriter* w) {
    if (w->bits > 0) {
        const int pad = 8 - w->bits;
        putBits(w, 0x7F, pad);   // ones pad, as the standard requires
        w->bits = 0;
    }
}

void buildTable(const uint8_t* bits, const uint8_t* values, int count, HuffTable* table) {
    memset(table, 0, sizeof(*table));
    uint16_t code = 0;
    int k = 0;
    for (int length = 1; length <= 16; ++length) {
        for (int i = 0; i < bits[length - 1]; ++i) {
            if (k >= count) return;
            table->code[values[k]] = code;
            table->size[values[k]] = uint8_t(length);
            ++code;
            ++k;
        }
        code <<= 1;
    }
}

float costab[8][8];
bool tablesReady;

void prepareTables() {
    if (tablesReady) return;
    const double pi = 3.14159265358979323846;
    for (int u = 0; u < 8; ++u) {
        for (int x = 0; x < 8; ++x) {
            costab[u][x] = float(cos((2.0 * x + 1.0) * u * pi / 16.0));
        }
    }
    tablesReady = true;
}

void forwardDct(const float* in, float* out) {
    float tmp[64];
    for (int y = 0; y < 8; ++y) {
        for (int u = 0; u < 8; ++u) {
            float sum = 0;
            for (int x = 0; x < 8; ++x) sum += in[y * 8 + x] * costab[u][x];
            tmp[y * 8 + u] = sum;
        }
    }
    // F(v,u) = 1/4 C(u) C(v) sum_y T(y,u) cos((2y+1) v pi / 16), with C(0) = 1/sqrt2.
    for (int u = 0; u < 8; ++u) {
        const float cu = (u == 0) ? 0.70710678f : 1.0f;
        for (int v = 0; v < 8; ++v) {
            float sum = 0;
            for (int y = 0; y < 8; ++y) sum += tmp[y * 8 + u] * costab[v][y];
            const float cv = (v == 0) ? 0.70710678f : 1.0f;
            out[v * 8 + u] = sum * 0.25f * cu * cv;
        }
    }
}

// One 8x8 block: DCT, quantise, then Huffman the zigzag scan.
void encodeBlock(BitWriter* w, const float* samples, const uint8_t* quant, const HuffTable* dc, const HuffTable* ac,
                 int* previousDc) {
    float coefficients[64];
    forwardDct(samples, coefficients);

    int level[64];
    for (int i = 0; i < 64; ++i) {
        const int natural = kZigzag[i];
        const int q = quant[natural];
        level[i] = int(lrintf(coefficients[natural] / float(q)));
    }

    // DC: difference against the previous block, then the usual size/value split.
    const int diff = level[0] - *previousDc;
    *previousDc = level[0];
    int magnitude = diff < 0 ? -diff : diff;
    int size = 0;
    while (magnitude) {
        ++size;
        magnitude >>= 1;
    }
    putBits(w, dc->code[size], dc->size[size]);
    if (size) putBits(w, uint32_t(diff < 0 ? diff - 1 : diff), size);

    // AC: run-length the zeros.
    int run = 0;
    for (int i = 1; i < 64; ++i) {
        if (level[i] == 0) {
            ++run;
            continue;
        }
        while (run > 15) {
            putBits(w, ac->code[0xF0], ac->size[0xF0]);   // ZRL
            run -= 16;
        }
        magnitude = level[i] < 0 ? -level[i] : level[i];
        size = 0;
        while (magnitude) {
            ++size;
            magnitude >>= 1;
        }
        const int symbol = (run << 4) | size;
        putBits(w, ac->code[symbol], ac->size[symbol]);
        putBits(w, uint32_t(level[i] < 0 ? level[i] - 1 : level[i]), size);
        run = 0;
    }
    if (run) putBits(w, ac->code[0x00], ac->size[0x00]);   // EOB
}

void scaleQuant(const uint8_t* base, int quality, uint8_t* out) {
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; ++i) {
        int value = (base[i] * scale + 50) / 100;
        if (value < 1) value = 1;
        if (value > 255) value = 255;
        out[i] = uint8_t(value);
    }
}

void writeSegment(BitWriter* w, uint8_t marker, const uint8_t* payload, size_t length) {
    putByte(w, 0xFF);
    putByte(w, marker);
    putByte(w, uint8_t((length + 2) >> 8));
    putByte(w, uint8_t((length + 2) & 0xFF));
    for (size_t i = 0; i < length; ++i) putByte(w, payload[i]);
}

}  // namespace

size_t jpeg_soft_encode(const uint16_t* rgb565, int width, int height, int quality, uint8_t* out, size_t capacity) {
    if (!rgb565 || !out || width < 16 || height < 8 || (width % 16) != 0 || (height % 8) != 0) return 0;

    prepareTables();

    HuffTable dcLuma, acLuma, dcChroma, acChroma;
    buildTable(kDcLumaBits, kDcLumaValues, 12, &dcLuma);
    buildTable(kAcLumaBits, kAcLumaValues, 162, &acLuma);
    buildTable(kDcChromaBits, kDcChromaValues, 12, &dcChroma);
    buildTable(kAcChromaBits, kAcChromaValues, 162, &acChroma);

    uint8_t lumaQuant[64], chromaQuant[64];
    scaleQuant(kLumaQuant, quality, lumaQuant);
    scaleQuant(kChromaQuant, quality, chromaQuant);

    BitWriter w = {};
    w.out = out;
    w.capacity = capacity;

    putByte(&w, 0xFF);
    putByte(&w, 0xD8);   // SOI

    // DQT: luminance then chroma, both in zigzag order.
    {
        uint8_t table[65];
        table[0] = 0x00;
        for (int i = 0; i < 64; ++i) table[i + 1] = lumaQuant[kZigzag[i]];
        writeSegment(&w, 0xDB, table, 65);
        table[0] = 0x01;
        for (int i = 0; i < 64; ++i) table[i + 1] = chromaQuant[kZigzag[i]];
        writeSegment(&w, 0xDB, table, 65);
    }

    // SOF0: baseline, 8-bit, three components, Y at 2x1 so a 16x8 MCU carries 2 Y blocks and one of each chroma.
    {
        uint8_t frame[15] = {8, uint8_t(height >> 8), uint8_t(height & 0xFF), uint8_t(width >> 8),
                             uint8_t(width & 0xFF), 3,
                             1, 0x21, 0,      // Y: h=2 v=1, quant table 0
                             2, 0x11, 1,      // Cb
                             3, 0x11, 1};     // Cr
        writeSegment(&w, 0xC0, frame, sizeof(frame));
    }

    // DHT: the four standard tables.
    {
        uint8_t table[1 + 16 + 162];
        table[0] = 0x00;
        memcpy(table + 1, kDcLumaBits, 16);
        memcpy(table + 17, kDcLumaValues, 12);
        writeSegment(&w, 0xC4, table, 1 + 16 + 12);
        table[0] = 0x10;
        memcpy(table + 1, kAcLumaBits, 16);
        memcpy(table + 17, kAcLumaValues, 162);
        writeSegment(&w, 0xC4, table, 1 + 16 + 162);
        table[0] = 0x01;
        memcpy(table + 1, kDcChromaBits, 16);
        memcpy(table + 17, kDcChromaValues, 12);
        writeSegment(&w, 0xC4, table, 1 + 16 + 12);
        table[0] = 0x11;
        memcpy(table + 1, kAcChromaBits, 16);
        memcpy(table + 17, kAcChromaValues, 162);
        writeSegment(&w, 0xC4, table, 1 + 16 + 162);
    }

    // SOS
    {
        uint8_t scan[10] = {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
        writeSegment(&w, 0xDA, scan, sizeof(scan));
    }

    int dcY = 0, dcCb = 0, dcCr = 0;
    const int mcusPerRow = width / 16;
    const int mcuRows = height / 8;

    float yBlock[64];
    float cbBlock[64];
    float crBlock[64];

    for (int my = 0; my < mcuRows; ++my) {
        for (int mx = 0; mx < mcusPerRow; ++mx) {
            // Luma: two 8x8 blocks side by side.
            for (int half = 0; half < 2; ++half) {
                for (int row = 0; row < 8; ++row) {
                    const uint16_t* line = rgb565 + (size_t)(my * 8 + row) * width + mx * 16 + half * 8;
                    for (int col = 0; col < 8; ++col) {
                        const uint16_t p = line[col];
                        const int r = ((p >> 11) & 0x1F) * 255 / 31;
                        const int g = ((p >> 5) & 0x3F) * 255 / 63;
                        const int b = (p & 0x1F) * 255 / 31;
                        yBlock[row * 8 + col] = float(((19595 * r + 38470 * g + 7471 * b) >> 16) - 128);
                    }
                }
                encodeBlock(&w, yBlock, lumaQuant, &dcLuma, &acLuma, &dcY);
            }

            // Chroma: one sample per two pixels horizontally.
            for (int row = 0; row < 8; ++row) {
                const uint16_t* line = rgb565 + (size_t)(my * 8 + row) * width + mx * 16;
                for (int col = 0; col < 8; ++col) {
                    int sumCb = 0, sumCr = 0;
                    for (int sub = 0; sub < 2; ++sub) {
                        const uint16_t p = line[col * 2 + sub];
                        const int r = ((p >> 11) & 0x1F) * 255 / 31;
                        const int g = ((p >> 5) & 0x3F) * 255 / 63;
                        const int b = (p & 0x1F) * 255 / 31;
                        sumCb += (-11059 * r - 21709 * g + 32768 * b) >> 16;
                        sumCr += (32768 * r - 27439 * g - 5329 * b) >> 16;
                    }
                    cbBlock[row * 8 + col] = float(sumCb / 2);
                    crBlock[row * 8 + col] = float(sumCr / 2);
                }
            }
            encodeBlock(&w, cbBlock, chromaQuant, &dcChroma, &acChroma, &dcCb);
            encodeBlock(&w, crBlock, chromaQuant, &dcChroma, &acChroma, &dcCr);
        }
    }

    flushBits(&w);
    putByte(&w, 0xFF);
    putByte(&w, 0xD9);   // EOI

    if (w.overflow) return 0;
    return w.written;
}
