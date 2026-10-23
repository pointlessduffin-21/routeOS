// The Orbit calculator, history and notes screens laid out for the 800x1280 portrait panel.
//
// The ESP32 build draws these screens from an EEZ Studio export sized 240x135. This is the same design (same colours,
// texts, keys and widget names, so ui_bindings.cpp is unchanged) at the size of the real panel. It stands in for the
// EEZ-generated screens.c and ui.c, and uses their screens.h for the `objects` structure.
#include "screens.h"
#include "ui.h"
#include "lvgl.h"

objects_t objects;

namespace {
constexpr int W = 800, H = 1280, MARGIN = 32;
constexpr uint32_t BG = 0x0a1020, TEXT = 0xedf4ff, MINT = 0x7ef3cb, PANEL = 0x142137, EDGE = 0x2a3b56, MUTED = 0x8296b4;
int16_t current = -1;

lv_obj_t* screen() {
    lv_obj_t* s = lv_obj_create(NULL);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s, lv_color_hex(TEXT), 0);
    lv_obj_set_style_pad_all(s, 0, 0);
    lv_obj_set_style_border_width(s, 0, 0);
    return s;
}

lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t colour, int x, int y, int w = 0) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text_static(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    if (w) { lv_obj_set_width(l, w); }
    lv_obj_set_pos(l, x, y);
    return l;
}

lv_obj_t* dot(lv_obj_t* parent, int x, int y) {
    lv_obj_t* d = lv_obj_create(parent);
    lv_obj_remove_flag(d, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(d, 18, 18);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_style_bg_color(d, lv_color_hex(MINT), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, 9, 0);
    return d;
}

// A small navigation button in the header row. Returns the button; the label goes to *text.
lv_obj_t* nav(lv_obj_t* parent, const char* caption, int x, int w, lv_obj_t** text) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_pos(b, x, 22);
    lv_obj_set_size(b, w, 64);
    lv_obj_set_style_bg_color(b, lv_color_hex(PANEL), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(EDGE), 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x33506e), LV_STATE_PRESSED);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text_static(l, caption);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(MUTED), 0);
    lv_obj_center(l);
    if (text) *text = l;
    return b;
}

void header(lv_obj_t* parent, const char* title, lv_obj_t** dotOut, lv_obj_t** titleOut) {
    *dotOut = dot(parent, MARGIN, 45);
    *titleOut = label(parent, title, &lv_font_montserrat_28, MINT, MARGIN + 32, 36, 300);
}

void style_textarea(lv_obj_t* t, const lv_font_t* font) {
    lv_obj_set_style_text_font(t, font, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(PANEL), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(0x2c405d), 0);
    lv_obj_set_style_border_width(t, 2, 0);
    lv_obj_set_style_radius(t, 20, 0);
    lv_obj_set_style_pad_left(t, 24, 0);
    lv_obj_set_style_pad_right(t, 24, 0);
    lv_obj_set_style_pad_top(t, 22, 0);
    lv_obj_set_style_pad_bottom(t, 12, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(TEXT), 0);
}

lv_obj_t* history_row(lv_obj_t* parent, int y, lv_obj_t** expr, lv_obj_t** answer, const char* firstText) {
    lv_obj_t* row = lv_button_create(parent);
    lv_obj_set_pos(row, MARGIN, y);
    lv_obj_set_size(row, W - 2 * MARGIN, 150);
    lv_obj_set_style_bg_color(row, lv_color_hex(PANEL), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x263650), 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_radius(row, 22, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1c2c47), LV_STATE_PRESSED);
    *expr = label(row, firstText, &lv_font_montserrat_24, MUTED, 24, 24, 700);
    *answer = label(row, "", &lv_font_montserrat_40, MINT, 24, 74, 700);
    lv_obj_set_style_text_align(*answer, LV_TEXT_ALIGN_RIGHT, 0);
    return row;
}
}  // namespace

extern "C" {

void create_screen_calculator() {
    lv_obj_t* s = objects.calculator = screen();
    header(s, "ORBIT / CALC", &objects.calculator_dot, &objects.calculator_title);
    objects.calc_history = nav(s, "HISTORY", 434, 190, &objects.calc_history_label);
    objects.calc_notes = nav(s, "NOTES", 640, 128, &objects.calc_notes_label);

    objects.result_caption = label(s, "LIVE RESULT", &lv_font_montserrat_24, MUTED, MARGIN, 130, 400);
    objects.result = label(s, "0", &lv_font_montserrat_48, MINT, MARGIN, 172, W - 2 * MARGIN);
    objects.calc_hint = label(s, "TYPE OR TAP", &lv_font_montserrat_24, MUTED, MARGIN, 250, W - 2 * MARGIN);

    lv_obj_t* e = objects.expression = lv_textarea_create(s);
    lv_obj_set_pos(e, MARGIN, 306);
    lv_obj_set_size(e, W - 2 * MARGIN, 110);
    lv_textarea_set_accepted_chars(e, "0123456789.eE +-*/()%^");
    lv_textarea_set_max_length(e, 64);
    lv_textarea_set_placeholder_text(e, "0");
    lv_textarea_set_one_line(e, true);
    lv_textarea_set_password_mode(e, false);
    style_textarea(e, &lv_font_montserrat_40);

    lv_obj_t* k = objects.keypad = lv_buttonmatrix_create(s);
    lv_obj_set_pos(k, MARGIN, 446);
    lv_obj_set_size(k, W - 2 * MARGIN, 790);
    static const char* map[25] = {"AC", "DEL", "%", "/", "\n", "7", "8", "9", "*", "\n", "4", "5", "6", "-", "\n",
                                  "1", "2", "3", "+", "\n", "+/-", "0", ".", "=", NULL};
    static lv_buttonmatrix_ctrl_t ctrl[20];
    for (auto& c : ctrl) c = (lv_buttonmatrix_ctrl_t)(1 | LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG);
    lv_buttonmatrix_set_map(k, map);
    lv_buttonmatrix_set_ctrl_map(k, ctrl);
    lv_obj_set_style_bg_opa(k, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(k, 0, 0);
    lv_obj_set_style_pad_all(k, 0, 0);
    lv_obj_set_style_pad_row(k, 14, 0);
    lv_obj_set_style_pad_column(k, 14, 0);
    lv_obj_set_style_bg_color(k, lv_color_hex(PANEL), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(k, lv_color_hex(TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_48, LV_PART_ITEMS);
    lv_obj_set_style_border_width(k, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(k, 26, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(k, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(k, lv_color_hex(0x33506e), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(k, lv_color_hex(MINT), LV_PART_ITEMS | LV_STATE_PRESSED);
}

void create_screen_history() {
    lv_obj_t* s = objects.history = screen();
    header(s, "HISTORY", &objects.history_dot, &objects.history_title);
    objects.history_clear = nav(s, "CLEAR", 270, 140, &objects.history_clear_label);
    objects.history_calc = nav(s, "CALC", 424, 130, &objects.history_calc_label);
    objects.history_notes = nav(s, "NOTES", 568, 200, &objects.history_notes_label);
    objects.history_row_0 = history_row(s, 130, &objects.history_expr_0, &objects.history_answer_0, "No calculations yet");
    objects.history_row_1 = history_row(s, 300, &objects.history_expr_1, &objects.history_answer_1, "");
    objects.history_row_2 = history_row(s, 470, &objects.history_expr_2, &objects.history_answer_2, "");
    objects.history_tip = label(s, "TAP A RESULT TO USE IT AGAIN", &lv_font_montserrat_24, MUTED, MARGIN, 660, W - 2 * MARGIN);
}

void create_screen_notes() {
    lv_obj_t* s = objects.notes = screen();
    header(s, "NOTES", &objects.notes_dot, &objects.notes_title);
    objects.notes_calc = nav(s, "CALC", 434, 130, &objects.notes_calc_label);
    objects.notes_history = nav(s, "HISTORY", 578, 190, &objects.notes_history_label);
    lv_obj_t* t = objects.notes_input = lv_textarea_create(s);
    lv_obj_set_pos(t, MARGIN, 120);
    lv_obj_set_size(t, W - 2 * MARGIN, 470);
    lv_textarea_set_max_length(t, 512);
    lv_textarea_set_placeholder_text(t, "Write a note");
    style_textarea(t, &lv_font_montserrat_36);
    lv_obj_t* kb = objects.notes_keyboard = lv_keyboard_create(s);
    // lv_keyboard_create() ends with lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, 0), so coordinates set afterwards are
    // treated as offsets from the bottom centre and land below the panel. Clear that alignment first.
    lv_obj_set_align(kb, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(kb, MARGIN, 620);
    lv_obj_set_size(kb, W - 2 * MARGIN, 600);
    lv_keyboard_set_textarea(kb, t);
    lv_obj_set_style_bg_color(kb, lv_color_hex(BG), 0);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(PANEL), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(TEXT), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 14, LV_PART_ITEMS);
}

void tick_screen_calculator() {}
void tick_screen_history() {}
void tick_screen_notes() {}

void tick_screen_by_id(enum ScreensEnum) {}
void tick_screen(int) {}

void create_screens() {
    create_screen_calculator();
    create_screen_history();
    create_screen_notes();
}

void loadScreen(enum ScreensEnum screenId) {
    current = screenId - 1;
    lv_screen_load(((lv_obj_t**)&objects)[current]);
}

void ui_init() {
    create_screens();
    loadScreen(SCREEN_ID_CALCULATOR);
}

void ui_tick() {}

}  // extern "C"
