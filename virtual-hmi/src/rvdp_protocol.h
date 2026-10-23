#pragma once
#include <stddef.h>
#include <stdint.h>

// Geometry belongs to the build, not the protocol: the 240x135 board and the 800x1280 panel share this header.
// A build overrides these with RVDP_WIDTH / RVDP_HEIGHT / RVDP_STRIP_ROWS.
#ifndef RVDP_WIDTH
#define RVDP_WIDTH 240
#endif
#ifndef RVDP_HEIGHT
#define RVDP_HEIGHT 135
#endif
#ifndef RVDP_STRIP_ROWS
#define RVDP_STRIP_ROWS 16
#endif

namespace rvdp {
constexpr uint16_t WIDTH = RVDP_WIDTH, HEIGHT = RVDP_HEIGHT, STRIP_ROWS = RVDP_STRIP_ROWS;
constexpr size_t TILE_BYTES = WIDTH * STRIP_ROWS * 2;
constexpr size_t MAX_PACKET = 16 + 17 + TILE_BYTES;
enum Type : uint8_t { HELLO=1, DISPLAY_INFO=2, RECT=3, FRAME_END=4,
  RESYNC=5, POINTER=6, KEY=7, PING=8, PONG=9, ERROR=10, CONTROL_STATE=11 };
inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0])<<8 | p[1]; }
inline uint32_t u32(const uint8_t* p) { return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3]; }
inline void put16(uint8_t* p,uint16_t v) { p[0]=v>>8; p[1]=v; }
inline void put32(uint8_t* p,uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
struct Packet { uint32_t epoch; uint16_t length; uint8_t data[MAX_PACKET]; };
struct Input { uint16_t x,y; bool down; uint32_t epoch; };
struct KeyInput { uint32_t key; bool down; uint32_t epoch; };
inline bool validKey(uint32_t key) {
  return (key>=32 && key<=126) || key==0xff08 || key==0xff09 || key==0xff0d || key==0xff1b ||
    key==0xff50 || key==0xff51 || key==0xff53 || key==0xff55 || key==0xff56 || key==0xff57 || key==0xffff;
}
bool decode(const uint8_t* p,size_t n,uint8_t& type,uint32_t& sequence,const uint8_t*& payload,size_t& length);
void header(uint8_t* p,uint8_t type,uint32_t sequence,uint32_t length,uint8_t minor=0);
}
