// Application adapter for the real EEZ export. Never edit eez_generated files.
#include <Arduino.h>
#include <ctype.h>
#include "eez_generated/ui.h"
#include "eez_generated/screens.h"
#include "ui_bindings.h"
#include "calculator.h"
#include "hmi_transport.h"

namespace {
struct History {char expression[65],answer[32];};
History history[3]{};
uint16_t historyCount=0;
uint8_t page=0;
bool editing=false,justEvaluated=false;
DemoState snapshot{};
portMUX_TYPE snapshotLock=portMUX_INITIALIZER_UNLOCKED;
const char* pageNames[]={"calculator","history","notes"};
lv_obj_t* historyExpressions[3];
lv_obj_t* historyAnswers[3];

void publish() {
  DemoState state{};
  strlcpy(state.page,pageNames[page],sizeof(state.page));
  strlcpy(state.expression,lv_textarea_get_text(objects.expression),sizeof(state.expression));
  strlcpy(state.result,lv_label_get_text(objects.result),sizeof(state.result));
  state.historyCount=historyCount;
  state.noteLength=strlen(lv_textarea_get_text(objects.notes_input));
  portENTER_CRITICAL(&snapshotLock);snapshot=state;portEXIT_CRITICAL(&snapshotLock);
}
void preview() {
  double answer;const char* error=nullptr;
  if(calculate(lv_textarea_get_text(objects.expression),answer,error)) {
    char text[32];snprintf(text,sizeof(text),"%.10g",answer==0?0:answer);
    lv_label_set_text(objects.result,text);
    lv_label_set_text(objects.result_caption,"LIVE RESULT");
    lv_label_set_text(objects.calc_hint,"ENTER = SAVE");
    lv_obj_set_style_text_color(objects.result,lv_color_hex(0x7ef3cb),0);
  } else {
    lv_label_set_text(objects.result,"ERR");
    lv_label_set_text(objects.result_caption,"CHECK INPUT");
    lv_label_set_text(objects.calc_hint,error);
    lv_obj_set_style_text_color(objects.result,lv_color_hex(0xffa5b5),0);
  }
  publish();
}
void expressionChanged(lv_event_t*) {if(!editing){justEvaluated=false;preview();}}
void notesChanged(lv_event_t*) {publish();}
void setExpression(const char* text) {
  editing=true;lv_textarea_set_text(objects.expression,text);lv_textarea_set_cursor_pos(objects.expression,LV_TEXTAREA_CURSOR_LAST);editing=false;
  preview();
}
void updateHistory() {
  for(unsigned i=0;i<3;i++) {
    lv_label_set_text(historyExpressions[i],i<historyCount?history[i].expression:(i==0?"Your next idea starts here":""));
    lv_label_set_text(historyAnswers[i],i<historyCount?history[i].answer:"");
  }
}
void showPage(unsigned target) {
  page=target%3;
  loadScreen(static_cast<ScreensEnum>(page+1));
  if(page==1)updateHistory();
  if(page==2)lv_textarea_set_cursor_pos(objects.notes_input,LV_TEXTAREA_CURSOR_LAST);
  publish();
}
void navigate(lv_event_t* event) {showPage(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));}
void clearHistory(lv_event_t*) {historyCount=0;memset(history,0,sizeof(history));updateHistory();publish();}
void reuseHistory(lv_event_t* event) {
  unsigned i=reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if(i<historyCount){setExpression(history[i].answer);justEvaluated=true;showPage(0);}
}
void equals() {
  const char* expression=lv_textarea_get_text(objects.expression);
  double answer;const char* error=nullptr;
  if(!*expression || !calculate(expression,answer,error)){preview();return;}
  for(unsigned i=2;i>0;--i)history[i]=history[i-1];
  strlcpy(history[0].expression,expression,sizeof(history[0].expression));
  snprintf(history[0].answer,sizeof(history[0].answer),"%.10g",answer==0?0:answer);
  if(historyCount<3)historyCount++;
  char value[32];strlcpy(value,history[0].answer,sizeof(value));setExpression(value);justEvaluated=true;
  lv_label_set_text(objects.result_caption,"SAVED");
  Serial.printf("[hmi] calculator result=%s history=%u\n",value,historyCount);
  publish();
}
void character(char c) {
  if(justEvaluated && (isdigit(static_cast<unsigned char>(c)) || c=='.' || c=='('))setExpression("");
  justEvaluated=false;
  lv_textarea_add_char(objects.expression,c);
}
void changeSign() {
  char text[65];strlcpy(text,lv_textarea_get_text(objects.expression),sizeof(text));
  size_t n=strlen(text),start=n;
  while(start>0 && (isdigit(static_cast<unsigned char>(text[start-1])) || text[start-1]=='.'))--start;
  if(n==0){setExpression("-");return;}
  if(start>0 && text[start-1]=='-' && (start==1 || strchr("(+-*/^",text[start-2])))memmove(text+start-1,text+start,n-start+1);
  else if(n<64){memmove(text+start+1,text+start,n-start+1);text[start]='-';}
  justEvaluated=false;setExpression(text);
}
void keypad(lv_event_t*) {
  uint16_t selected=lv_btnmatrix_get_selected_btn(objects.keypad);
  if(selected==LV_BTNMATRIX_BTN_NONE)return;
  const char* key=lv_btnmatrix_get_btn_text(objects.keypad,selected);
  if(!strcmp(key,"AC")){setExpression("");justEvaluated=false;}
  else if(!strcmp(key,"DEL")){justEvaluated=false;lv_textarea_del_char(objects.expression);}
  else if(!strcmp(key,"+/-"))changeSign();
  else if(!strcmp(key,"="))equals();
  else character(key[0]);
}
void keyColours(lv_event_t* event) {
  auto* draw=static_cast<lv_obj_draw_part_dsc_t*>(lv_event_get_param(event));
  if(draw->part!=LV_PART_ITEMS || !draw->rect_dsc || !draw->label_dsc)return;
  unsigned id=draw->id;
  if(id==19) {draw->rect_dsc->bg_color=lv_color_hex(0x7ef3cb);draw->label_dsc->color=lv_color_hex(0x0a1820);}
  else if(id%4==3){draw->rect_dsc->bg_color=lv_color_hex(0x302849);draw->label_dsc->color=lv_color_hex(0xc8b2ff);}
  else if(id==0 || id==1){draw->rect_dsc->bg_color=lv_color_hex(0x26324a);draw->label_dsc->color=lv_color_hex(0xffb1bb);}
}
void keyInput(uint32_t key) {
  if(key==0xff09 || key==0xff56){showPage(page+1);return;}
  if(key==0xff55){showPage(page+2);return;}
  if(key==0xff1b){if(page==0){setExpression("");justEvaluated=false;}else showPage(0);return;}
  if(page==1){if(key==0xff0d && historyCount){setExpression(history[0].answer);showPage(0);}return;}
  lv_obj_t* target=page==2?objects.notes_input:objects.expression;
  if(key==0xff0d){if(page==0)equals();else lv_textarea_add_char(target,'\n');return;}
  if(key==0xff08)lv_textarea_del_char(target);
  else if(key==0xffff)lv_textarea_del_char_forward(target);
  else if(key==0xff51)lv_textarea_cursor_left(target);
  else if(key==0xff53)lv_textarea_cursor_right(target);
  else if(key==0xff50)lv_textarea_set_cursor_pos(target,0);
  else if(key==0xff57)lv_textarea_set_cursor_pos(target,LV_TEXTAREA_CURSOR_LAST);
  else if(key>=32 && key<=126){if(page==0)character(char(key));else lv_textarea_add_char(target,key);}
}
}

void demoSnapshot(DemoState& state) {
  portENTER_CRITICAL(&snapshotLock);state=snapshot;portEXIT_CRITICAL(&snapshotLock);
}
void demoInit() {
  ui_init();
  historyExpressions[0]=objects.history_expr_0;historyExpressions[1]=objects.history_expr_1;historyExpressions[2]=objects.history_expr_2;
  historyAnswers[0]=objects.history_answer_0;historyAnswers[1]=objects.history_answer_1;historyAnswers[2]=objects.history_answer_2;
  const struct{lv_obj_t* object;unsigned page;} nav[]={
    {objects.calc_history,1},{objects.calc_notes,2},{objects.history_calc,0},{objects.history_notes,2},{objects.notes_calc,0},{objects.notes_history,1}};
  for(const auto& button:nav)lv_obj_add_event_cb(button.object,navigate,LV_EVENT_CLICKED,reinterpret_cast<void*>(uintptr_t(button.page)));
  lv_obj_add_event_cb(objects.expression,expressionChanged,LV_EVENT_VALUE_CHANGED,nullptr);
  lv_obj_add_event_cb(objects.notes_input,notesChanged,LV_EVENT_VALUE_CHANGED,nullptr);
  lv_obj_add_event_cb(objects.keypad,keypad,LV_EVENT_VALUE_CHANGED,nullptr);
  lv_obj_add_event_cb(objects.keypad,keyColours,LV_EVENT_DRAW_PART_BEGIN,nullptr);
  lv_obj_add_event_cb(objects.history_clear,clearHistory,LV_EVENT_CLICKED,nullptr);
  lv_obj_t* rows[]={objects.history_row_0,objects.history_row_1,objects.history_row_2};
  for(unsigned i=0;i<3;i++)lv_obj_add_event_cb(rows[i],reuseHistory,LV_EVENT_CLICKED,reinterpret_cast<void*>(uintptr_t(i)));
  for(unsigned i=0;i<3;i++){lv_obj_clear_flag(historyExpressions[i],LV_OBJ_FLAG_CLICKABLE);lv_obj_clear_flag(historyAnswers[i],LV_OBJ_FLAG_CLICKABLE);}
  lv_textarea_set_cursor_click_pos(objects.expression,true);
  lv_textarea_set_cursor_click_pos(objects.notes_input,true);
  updateHistory();preview();
}
void demoTick() {
  rvdp::KeyInput key;
  for(unsigned i=0;i<16 && xQueueReceive(hmi::keys,&key,0)==pdTRUE;i++)
    if(key.down && hmi::inputReady && key.epoch==hmi::inputEpoch)keyInput(key.key);
  ui_tick();
}
