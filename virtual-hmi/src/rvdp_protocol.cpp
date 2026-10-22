#include "rvdp_protocol.h"
#include <string.h>

bool rvdp::decode(const uint8_t* p,size_t n,uint8_t& type,uint32_t& sequence,const uint8_t*& payload,size_t& length) {
  if(n<16 || n>1024 || memcmp(p,"RVDP",4) || p[4]!=1 || p[5]>1 || p[7]!=0) return false;
  type=p[6]; sequence=u32(p+8); length=u32(p+12);
  if(length!=n-16) return false;
  payload=p+16;
  return type==HELLO || type==RESYNC || type==POINTER || type==KEY || type==PING || type==PONG;
}
void rvdp::header(uint8_t* p,uint8_t type,uint32_t sequence,uint32_t length,uint8_t minor) {
  memcpy(p,"RVDP",4); p[4]=1; p[5]=minor; p[6]=type; p[7]=0; put32(p+8,sequence);put32(p+12,length);
}
