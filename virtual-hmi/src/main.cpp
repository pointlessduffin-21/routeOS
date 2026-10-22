#include <Arduino.h>
#include <lvgl.h>
#include "hmi_transport.h"
#include "ui_bindings.h"
#include <routeos_core.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

static lv_color_t pixels[rvdp::WIDTH*rvdp::STRIP_ROWS];
static lv_disp_draw_buf_t drawBuffer;
static lv_disp_drv_t driver;
static lv_indev_drv_t inputDriver;
static lv_disp_t* display;
static rvdp::Packet tile;
static uint32_t batch=0,batchEpoch=0;
static uint32_t completedEpoch=0;
static uint16_t rectCount=0;
static bool damaged=false,full=false;
static rvdp::Input pointer{0,0,false,0};
static uint32_t pointerAt=0;
#ifndef ROUTEOS_MANAGE
#define ROUTEOS_MANAGE 0
#endif
// Compiled-in modules, reported to Termz. RVDP is part of this image; Tailscale is not.
static const routeos::Module kModules[]={{"core",routeos::CORE_VERSION},{"rvdp","1.1"},{"drivers","1.0.0"}};

static void flush(lv_disp_drv_t* drv,const lv_area_t* area,lv_color_t* colour) {
  // A new session/resync must begin with a complete snapshot. Automatic LVGL
  // dirty redraws can run while stale queue entries are still draining.
  if(!hmi::ready || (!full && completedEpoch!=hmi::epoch)) {lv_disp_flush_ready(drv);return;}
  if(rectCount==0) {batch++;batchEpoch=hmi::epoch.load();damaged=false;}
  uint16_t w=area->x2-area->x1+1,h=area->y2-area->y1+1;
  size_t bytes=size_t(w)*h*2;
  if(bytes>rvdp::TILE_BYTES || batchEpoch!=hmi::epoch) damaged=true;
  if(!damaged) {
    tile.epoch=batchEpoch;tile.length=16+17+bytes;
    rvdp::header(tile.data,rvdp::RECT,0,17+bytes);uint8_t* p=tile.data+16;
    rvdp::put32(p,batch);rvdp::put16(p+4,area->x1);rvdp::put16(p+6,area->y1);
    rvdp::put16(p+8,w);rvdp::put16(p+10,h);p[12]=1;rvdp::put32(p+13,bytes);
    for(size_t i=0;i<size_t(w)*h;i++)rvdp::put16(p+17+i*2,colour[i].full);
    if(xQueueSend(hmi::outgoing,&tile,0)!=pdTRUE) {damaged=true;hmi::drops++;}
  }
  ++rectCount;
  if(lv_disp_flush_is_last(drv)) {
    if(!damaged) {
      tile.epoch=batchEpoch;tile.length=27;rvdp::header(tile.data,rvdp::FRAME_END,0,11);
      rvdp::put32(tile.data+16,batch);rvdp::put16(tile.data+20,rectCount);tile.data[22]=full;
      rvdp::put32(tile.data+23,millis());
      if(xQueueSend(hmi::outgoing,&tile,0)!=pdTRUE) {damaged=true;hmi::drops++;}
      else {hmi::batches++;if(full)completedEpoch=batchEpoch;}
    }
    if(damaged) hmi::overflow=true;
    rectCount=0;full=false;
  }
  // Queue owns a copy, never an LVGL buffer pointer.
  lv_disp_flush_ready(drv);
}
static void readInput(lv_indev_drv_t*,lv_indev_data_t* data) {
  rvdp::Input next;
  // Preserve down/up transitions across reads, rather than collapsing a fast click.
  if(xQueueReceive(hmi::inputs,&next,0)==pdTRUE) {
    pointer=next;pointerAt=millis();
  }
  if(pointer.epoch!=hmi::inputEpoch || !hmi::inputReady || millis()-pointerAt>2000) pointer.down=false;
  data->point.x=pointer.x;data->point.y=pointer.y;
  data->state=pointer.down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
  data->continue_reading=uxQueueMessagesWaiting(hmi::inputs)>0;
}
void setup() {
  Serial.begin(115200);delay(200);
  Serial.printf("[hmi] chip=%s rev=%u cores=%u flash=%u sdk=%s LVGL=%d.%d.%d\n",ESP.getChipModel(),ESP.getChipRevision(),ESP.getChipCores(),ESP.getFlashChipSize(),ESP.getSdkVersion(),LVGL_VERSION_MAJOR,LVGL_VERSION_MINOR,LVGL_VERSION_PATCH);
  hmi::printMetrics("boot");
  lv_init();lv_disp_draw_buf_init(&drawBuffer,pixels,nullptr,rvdp::WIDTH*rvdp::STRIP_ROWS);
  lv_disp_drv_init(&driver);driver.hor_res=rvdp::WIDTH;driver.ver_res=rvdp::HEIGHT;driver.draw_buf=&drawBuffer;driver.flush_cb=flush;
  display=lv_disp_drv_register(&driver);
  lv_indev_drv_init(&inputDriver);inputDriver.type=LV_INDEV_TYPE_POINTER;inputDriver.read_cb=readInput;lv_indev_drv_register(&inputDriver);
  routeos::begin("routeos-orbit-hmi","1.0.0",kModules,sizeof(kModules)/sizeof(kModules[0]),ROUTEOS_MANAGE);
  hmi::startNetwork();demoInit();
  hmi::printMetrics("lvgl-ready");
}
void loop() {
  static uint32_t lastTick=millis();uint32_t now=millis();lv_tick_inc(now-lastTick);lastTick=now;
  uint32_t start=micros();
  demoTick();
  if(hmi::ready && hmi::refresh.exchange(false)) {
    // Old queued generations are filtered by the sender. Wait until they drain so
    // the complete nine-strip snapshot plus FRAME_END fits the ten-slot queue.
    if(uxQueueMessagesWaiting(hmi::outgoing)==0) {
      rectCount=0;full=true;lv_obj_invalidate(lv_scr_act());lv_refr_now(display);
    } else hmi::refresh=true;
  }
  // Preserve LVGL's dirty regions until the previous batch has drained.
  // Rendering into a full queue can cancel a snapshot before FRAME_END reaches
  // the viewer, repeatedly restarting the initial-screen handshake.
  if(uxQueueMessagesWaiting(hmi::outgoing)==0) lv_timer_handler();
  hmi::uiMicros=micros()-start;hmi::uiStack=uxTaskGetStackHighWaterMark(nullptr);
  delay(5);
}
