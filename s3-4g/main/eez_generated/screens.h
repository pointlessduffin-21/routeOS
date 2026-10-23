#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Screens

enum ScreensEnum {
    _SCREEN_ID_FIRST = 1,
    SCREEN_ID_CALCULATOR = 1,
    SCREEN_ID_HISTORY = 2,
    SCREEN_ID_NOTES = 3,
    _SCREEN_ID_LAST = 3
};

typedef struct _objects_t {
    lv_obj_t *calculator;
    lv_obj_t *history;
    lv_obj_t *notes;
    lv_obj_t *calculator_dot;
    lv_obj_t *calculator_title;
    lv_obj_t *calc_history;
    lv_obj_t *calc_history_label;
    lv_obj_t *calc_notes;
    lv_obj_t *calc_notes_label;
    lv_obj_t *result_caption;
    lv_obj_t *result;
    lv_obj_t *calc_hint;
    lv_obj_t *expression;
    lv_obj_t *keypad;
    lv_obj_t *history_dot;
    lv_obj_t *history_title;
    lv_obj_t *history_clear;
    lv_obj_t *history_clear_label;
    lv_obj_t *history_calc;
    lv_obj_t *history_calc_label;
    lv_obj_t *history_notes;
    lv_obj_t *history_notes_label;
    lv_obj_t *history_row_0;
    lv_obj_t *history_expr_0;
    lv_obj_t *history_answer_0;
    lv_obj_t *history_row_1;
    lv_obj_t *history_expr_1;
    lv_obj_t *history_answer_1;
    lv_obj_t *history_row_2;
    lv_obj_t *history_expr_2;
    lv_obj_t *history_answer_2;
    lv_obj_t *history_tip;
    lv_obj_t *notes_dot;
    lv_obj_t *notes_title;
    lv_obj_t *notes_calc;
    lv_obj_t *notes_calc_label;
    lv_obj_t *notes_history;
    lv_obj_t *notes_history_label;
    lv_obj_t *notes_input;
    lv_obj_t *notes_keyboard;
} objects_t;

extern objects_t objects;

void create_screen_calculator();
void tick_screen_calculator();

void create_screen_history();
void tick_screen_history();

void create_screen_notes();
void tick_screen_notes();

void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();

#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/