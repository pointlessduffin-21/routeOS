#include "hmi_transport.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include "web_assets.h"
#include "ui_bindings.h"
#include <routeos_core.h>
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef HMI_WIFI_SSID
#define HMI_WIFI_SSID ""
#define HMI_WIFI_PASSWORD ""
#endif
#ifndef HMI_ALLOW_CONTROL
#define HMI_ALLOW_CONTROL 0
#endif

namespace hmi {
QueueHandle_t outgoing, inputs, keys;
std::atomic<uint32_t> epoch{0}, drops{0}, sentBytes{0}, batches{0}, uiMicros{0}, uiStack{0};
std::atomic<bool> ready{false}, refresh{false}, overflow{false};
std::atomic<uint32_t> inputEpoch{0};
std::atomic<bool> inputReady{false};
static StaticQueue_t inputStruct,keyStruct;
alignas(4) static uint8_t inputStorage[16*sizeof(rvdp::Input)];
alignas(4) static uint8_t keyStorage[32*sizeof(rvdp::KeyInput)];
static routeos::Server http(80);      // a WebServer that survives half-sent and stalled requests
static WebSocketsServer ws(81);
static int viewer=-1;
static bool helloReceived=false,granted=false;
static uint8_t peerMinor=0;
static uint32_t tx=0,rx=0,connectedAt=0,lastRx=0,networkStack=0;
static rvdp::Packet sendPacket;

void printMetrics(const char* stage) {
  Serial.printf("[hmi] %s heap=%u min=%u largest=%u psram=%u reset=%d queue=%u drops=%u batches=%u bytes=%u ui_us=%u ui_stack=%u net_stack=%u\n",
    stage,ESP.getFreeHeap(),ESP.getMinFreeHeap(),heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),ESP.getPsramSize(),
    int(esp_reset_reason()),outgoing?uxQueueMessagesWaiting(outgoing):0,drops.load(),batches.load(),sentBytes.load(),uiMicros.load(),uiStack.load(),networkStack);
}
static void sendSmall(uint8_t type,const uint8_t* payload,size_t length) {
  uint8_t bytes[64];
  if(viewer<0 || length>48) return;
  rvdp::header(bytes,type,tx++,length,peerMinor); memcpy(bytes+16,payload,length);
  if(!ws.sendBIN(viewer,bytes,length+16)) ws.disconnect(viewer);
  else sentBytes+=length+16;
}
static void newGeneration() {
  ready=false;
  epoch.fetch_add(1);
  overflow=false;
  // Pixel-generation changes must not discard acknowledged keyboard input.
  refresh=true;
  ready=helloReceived;
}
static void onEvent(uint8_t client,WStype_t type,uint8_t* data,size_t length) {
  if(type==WStype_CONNECTED) {
    if(length!=5 || memcmp(data,"/rvdp",5)) {ws.disconnect(client);return;}
    if(viewer>=0) {
      const char message[]="routeOS display is busy. Close the other browser tab or iShareScreen session, then retry.";
      uint8_t rejection[16+2+sizeof(message)-1];
      rvdp::header(rejection,rvdp::ERROR,0,2+sizeof(message)-1);
      rvdp::put16(rejection+16,409);memcpy(rejection+18,message,sizeof(message)-1);
      routeos::log('W',"display viewer turned away: another viewer is connected");
      ws.sendBIN(client,rejection,sizeof(rejection));ws.disconnect(client);return;
    }
    routeos::log('I',"display viewer connected");
    viewer=client;helloReceived=false;granted=false;peerMinor=0;tx=rx=0;connectedAt=lastRx=millis();
    ready=false;inputReady=false;inputEpoch.fetch_add(1);epoch.fetch_add(1);xQueueReset(inputs);xQueueReset(keys);return;
  }
  if(client!=viewer) return;
  if(type==WStype_DISCONNECTED) {
    routeos::log('I',"display viewer disconnected");
    ready=false;inputReady=false;inputEpoch.fetch_add(1);epoch.fetch_add(1);viewer=-1;helloReceived=granted=false;xQueueReset(inputs);xQueueReset(keys);return;
  }
  if(type!=WStype_BIN) {
    if(type!=WStype_PING && type!=WStype_PONG) ws.disconnect(client);
    return;
  }
  uint8_t kind;uint32_t sequence;const uint8_t* p;size_t n;
  if(!rvdp::decode(data,length,kind,sequence,p,n) || sequence!=rx++) {ws.disconnect(client);return;}
  lastRx=millis();
  if(!helloReceived) {
    if(kind!=rvdp::HELLO || n!=13 || p[0]!=1 || p[1] || p[2]!=1 || p[3]>1 || rvdp::u32(p+4)!=1 ||
       rvdp::u32(p+8)<rvdp::TILE_BYTES || rvdp::u32(p+8)>16384 || p[12]>1) {ws.disconnect(client);return;}
    helloReceived=true;granted=HMI_ALLOW_CONTROL && p[12];inputReady=granted;peerMinor=p[3];
    uint8_t info[14];rvdp::put16(info,rvdp::WIDTH);rvdp::put16(info+2,rvdp::HEIGHT);info[4]=1;info[5]=0;
    rvdp::put32(info+6,epoch);rvdp::put32(info+10,HMI_ALLOW_CONTROL?(peerMinor?3:1):0);
    sendSmall(rvdp::DISPLAY_INFO,info,sizeof(info));uint8_t permission=granted;
    sendSmall(rvdp::CONTROL_STATE,&permission,1);newGeneration();return;
  }
  if(kind==rvdp::RESYNC && n==1 && p[0]<=2) {newGeneration();return;}
  if(kind==rvdp::PING && n==4) {sendSmall(rvdp::PONG,p,4);return;}
  if(kind==rvdp::PONG && n==4) return;
  if(data[5]!=peerMinor) {ws.disconnect(client);return;}
  if(kind==rvdp::KEY && peerMinor==1 && granted && n==5) {
    uint32_t key=rvdp::u32(p);
    if(!rvdp::validKey(key) || p[4]>1) {ws.disconnect(client);return;}
    rvdp::KeyInput input{key,bool(p[4]),inputEpoch.load()};
    if(xQueueSend(keys,&input,0)!=pdTRUE) {xQueueReset(keys);drops++;ws.disconnect(client);}
    return;
  }
  if(kind==rvdp::POINTER && granted && n==6) {
    uint16_t x=rvdp::u16(p),y=rvdp::u16(p+2);
    if(x>=rvdp::WIDTH || y>=rvdp::HEIGHT || p[4]>1 || p[5]>3 || (p[5]>=2 && p[4])) {ws.disconnect(client);return;}
    rvdp::Input input{x,y,bool(p[4]),inputEpoch.load()};
    if(xQueueSend(inputs,&input,0)!=pdTRUE) {
      xQueueReset(inputs);input.down=false;xQueueSend(inputs,&input,0);drops++;
    }
    return;
  }
  ws.disconnect(client);
}
static void network(void*) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // A network saved through the management API is tried first, then the one built into this firmware.
  if(!routeos::wifiBegin(HMI_WIFI_SSID,HMI_WIFI_PASSWORD)) {
    char password[17];snprintf(password,sizeof(password),"%08x%08x",esp_random(),esp_random());
    String ssid=String("routeOS-HMI-")+String(uint32_t(ESP.getEfuseMac()),HEX);
    WiFi.mode(WIFI_AP);WiFi.softAP(ssid.c_str(),password);
    // Physical USB possession is required to read the fresh development AP key.
    Serial.printf("[hmi] temporary AP %s key=%s URL=http://192.168.4.1/hmi\n",ssid.c_str(),password);
  }
  http.on("/hmi",HTTP_GET,[]{http.sendHeader("Cache-Control","no-store");http.send_P(200,"text/html; charset=utf-8",hmi_html);});
  http.on("/wire.js",HTTP_GET,[]{http.send_P(200,"text/javascript; charset=utf-8",hmi_wire_js);});
  http.on("/viewer.js",HTTP_GET,[]{http.send_P(200,"text/javascript; charset=utf-8",hmi_viewer_js);});
  http.on("/health",HTTP_GET,[]{
    DemoState state;demoSnapshot(state);
    char body[768];snprintf(body,sizeof(body),"{\"width\":240,\"height\":135,\"heap\":%u,\"min_heap\":%u,\"largest_block\":%u,\"psram\":%u,\"queue\":%u,\"drops\":%u,\"batches\":%u,\"bytes\":%u,\"ui_us\":%u,\"ui_stack\":%u,\"net_stack\":%u,\"control_enabled\":%s,\"page\":\"%s\",\"expression\":\"%s\",\"result\":\"%s\",\"history_count\":%u,\"note_length\":%u}",
      ESP.getFreeHeap(),ESP.getMinFreeHeap(),heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),ESP.getPsramSize(),uxQueueMessagesWaiting(outgoing),drops.load(),batches.load(),sentBytes.load(),uiMicros.load(),uiStack.load(),networkStack,HMI_ALLOW_CONTROL?"true":"false",state.page,state.expression,state.result,state.historyCount,state.noteLength);
    http.send(200,"application/json",body);
  });
  routeos::attach(http);
  http.begin();ws.begin();ws.onEvent(onEvent);
  uint32_t metricsAt=0;bool announced=false;
  while(true) {
    http.handleClient();ws.loop();
    if(WiFi.status()==WL_CONNECTED && !announced) {announced=true;routeos::onNetworkUp();Serial.printf("[hmi] IP=%s URL=http://%s/hmi WS=ws://%s:81/rvdp\n",WiFi.localIP().toString().c_str(),WiFi.localIP().toString().c_str(),WiFi.localIP().toString().c_str());printMetrics("wifi-ready");}
    if(WiFi.status()!=WL_CONNECTED) announced=false;
    if(viewer>=0 && ((!helloReceived && millis()-connectedAt>5000) || millis()-lastRx>20000)) ws.disconnect(viewer);
    if(overflow.exchange(false)) newGeneration();
    // Only this task touches WebSockets. Old generations are drained, never sent.
    if(xQueueReceive(outgoing,&sendPacket,0)==pdTRUE && ready && sendPacket.epoch==epoch) {
      rvdp::put32(sendPacket.data+8,tx++);
      sendPacket.data[5]=peerMinor;
      if(!ws.sendBIN(viewer,sendPacket.data,sendPacket.length)) ws.disconnect(viewer);
      else sentBytes+=sendPacket.length;
    }
    networkStack=uxTaskGetStackHighWaterMark(nullptr);
    if(millis()-metricsAt>=5000){metricsAt=millis();printMetrics("running");}
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
void startNetwork() {
  // ESP32's statically mapped DRAM is smaller than its total internal heap.
  // Allocate the fixed-capacity queue once, before Wi-Fi fragments the heap.
  outgoing=xQueueCreate(10,sizeof(rvdp::Packet));
  inputs=xQueueCreateStatic(16,sizeof(rvdp::Input),inputStorage,&inputStruct);
  keys=xQueueCreateStatic(32,sizeof(rvdp::KeyInput),keyStorage,&keyStruct);
  if(!outgoing || !inputs || !keys || xTaskCreatePinnedToCore(network,"hmi-network",8192,nullptr,1,nullptr,0)!=pdPASS) {
    Serial.println("[hmi] FATAL: cannot initialize transport");while(true)delay(1000);
  }
}
}
