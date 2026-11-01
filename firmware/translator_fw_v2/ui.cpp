// TranslateOS - LVGL 9.3 UI module (640x172 landscape).
// Rules (see app_api.h): every lv_* call runs in the LVGL task (ui_init, lv_timer callbacks, event callbacks).
// Other tasks only call ui_post_*, which copy into a fixed struct and xQueueSend (timeout 0).
// Exactly one screen object alive at any time. Font size / night mode = shared styles, never a rebuild.
#include <Arduino.h>
#include "ui.h"
#include "lvgl.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

extern "C" void lvgl_port_set_flipped(bool flipped);   // provided by lvgl_port.c

static const char* TAG = "ui";

// ============================================================================
// Language tables (index order = SET_LANG_A/B value)
// ============================================================================
const char* const LANG_NAMES[LANG_COUNT] = {
  "English", "Italian", "French", "Spanish", "German",
  "Portuguese", "Japanese", "Chinese", "Ukrainian", "Arabic"
};
const char* const LANG_CODES[LANG_COUNT] = {
  "en", "it", "fr", "es", "de", "pt", "ja", "zh", "uk", "ar"
};

// ============================================================================
// Geometry
// ============================================================================
#define SCR_W       640
#define SCR_H       172
#define SB_W        36                       // sidebar
#define REC_W       54                       // REC button column
#define CONTENT_W   (SCR_W - SB_W)           // 604
#define CHAT_W      (CONTENT_W - REC_W)      // 550
#define STRIP_H     30                       // Talk: language swap strip above the chat
#define TITLE_H     32                       // sub screens: title bar
#define ROW_H       48                       // settings rows
#define HOME_INFO_W 176                      // Home: clock / wifi / battery column
#define BUB_TXT_MAX 490                      // max label width inside a bubble

#define TXT_MAX     400                      // bytes per orig / trans text
#define TOAST_MAX   96
#define SSID_MAX    33
#define CHAT_MAX    20                       // fixed ring slots (>= max histPairs)
#define Q_LEN       16

// ============================================================================
// Palette (from the old UI / HTML mockup)
// ============================================================================
typedef struct { uint32_t bg, sb, text, border, card, row, hint; uint32_t selA, selB; } Pal;
static const Pal PAL_LT = { 0xffffff, 0xf5f5f7, 0x1c1c1e, 0xe0e4ec, 0xe8e8ed, 0xffffff, 0xc7c7cc, 0xEBF4FF, 0xedfbf1 };
static const Pal PAL_DK = { 0x0a0a0a, 0x111111, 0xf5f5f7, 0x2c2c2e, 0x2c2c2e, 0x1c1c1e, 0x636366, 0x0b2a4a, 0x0f3320 };

#define COL_BLUE    0x007AFF
#define COL_GREEN   0x30C759
#define COL_ORANGE  0xFF9500
#define COL_RED     0xFF3B30
#define COL_SUBTLE  0x8e8e93

static inline lv_color_t COL(uint32_t hex) { return lv_color_hex(hex); }
#define OF(x) ((lv_obj_flag_t)(x))
#define SEL(x) ((lv_style_selector_t)(x))

// ============================================================================
// Font sets for Font Size 1..4
// ============================================================================
static const lv_font_t* const FONT_BODY[4]  = { &lv_font_montserrat_14, &lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20 };
static const lv_font_t* const FONT_TITLE[4] = { &lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_24 };
static const lv_font_t* const FONT_CARD[4]  = { &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16, &lv_font_montserrat_18 };
static const lv_font_t* const FONT_SMALL[4] = { &lv_font_montserrat_12, &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_14 };

// ============================================================================
// Queue message (app tasks -> LVGL task)
// ============================================================================
enum { UM_STATE, UM_BUBBLE, UM_TOAST, UM_STATUS, UM_PROGRESS, UM_SAVER, UM_CLEAR, UM_FLIP, UM_SELFTEST };

typedef struct {
  uint8_t type;
  union {
    struct { uint8_t state; } st;
    struct { char orig[TXT_MAX + 1]; char trans[TXT_MAX + 1]; } bub;
    struct { uint8_t kind; uint32_t ms; char text[TOAST_MAX + 1]; } toast;
    struct { uint8_t wifi; int16_t rssi; int16_t batt; uint8_t q; char ssid[SSID_MAX + 1]; } status;
    struct { uint16_t el; uint16_t mx; } prog;
    struct { uint8_t on; } flag;
    struct { int16_t mode; int16_t cycles; } selftest;
  } u;
} UiMsg;

static QueueHandle_t g_q = NULL;
static UiMsg s_msg;                          // drain buffer (static: keep the LVGL task stack small)

// ============================================================================
// Helpers
// ============================================================================
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline const Pal& pal();

// UTF-8 safe bounded copy: never leaves half a code point at the end.
static void copy_utf8(char* dst, size_t cap, const char* src) {
  if (!src) { dst[0] = 0; return; }
  size_t n = strnlen(src, cap);
  if (n >= cap) {
    n = cap - 1;
    while (n > 0 && (((uint8_t)src[n]) & 0xC0) == 0x80) n--;   // src[n] is a continuation byte: step back to a start byte
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

static void set_text(lv_obj_t* l, const char* t) {
  if (l && strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static const char* lang_name(int i) { return LANG_NAMES[(i >= 0 && i < LANG_COUNT) ? i : 0]; }

// ============================================================================
// UI state mirrors (written only in the LVGL task)
// ============================================================================
static bool g_night = false;
static int  g_font_idx = 2;                  // 1..4
static const lv_font_t* g_fbody = &lv_font_montserrat_16;
static bool g_bt = false;                    // Bluetooth placeholder switch

static UiState   g_state = UI_IDLE;
static uint16_t  g_prog_el = 0, g_prog_mx = 0;
static bool      g_st_wifi = false;
static int       g_st_batt = -1;
static NetQuality g_st_q = NETQ_NONE;
static char      g_st_ssid[SSID_MAX + 1] = "";
static bool      g_saver_on = false;

static inline const Pal& pal() { return g_night ? PAL_DK : PAL_LT; }

// ============================================================================
// Serial log (non-blocking: a line is dropped when the USB-CDC tx buffer is full) + page audit
// ============================================================================
static void stlog(const char* fmt, ...) {
  char b[220];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof b - 1, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
  b[n++] = '\n';
  if (Serial.availableForWrite() >= n) Serial.write((const uint8_t*)b, (size_t)n);
}

// Walks the live widgets of a screen (the children of `skip` are not walked: that is scrollable content).
// bottom/right = lowest/rightmost edge, over = widgets sticking out of their parent (clipped on screen).
static void audit_walk(lv_obj_t* o, lv_obj_t* skip, int* bottom, int* right, int* over) {
  lv_area_t pa;
  lv_obj_get_coords(o, &pa);
  uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t* c = lv_obj_get_child(o, i);
    if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_area_t a;
    lv_obj_get_coords(c, &a);
    if (a.y2 + 1 > *bottom) *bottom = a.y2 + 1;
    if (a.x2 + 1 > *right)  *right  = a.x2 + 1;
    if (a.x1 < pa.x1 || a.y1 < pa.y1 || a.x2 > pa.x2 || a.y2 > pa.y2) (*over)++;
    if (c != skip) audit_walk(c, skip, bottom, right, over);
  }
}

static void page_audit(const char* name, lv_obj_t* scr, lv_obj_t* scroller, int cols) {
  lv_obj_update_layout(scr);
  int bottom = 0, right = 0, over = 0, scrollable = 0;
  audit_walk(scr, scroller, &bottom, &right, &over);
  if (scroller) {
    uint32_t n = lv_obj_get_child_count(scroller);
    if (n) {
      lv_area_t a;
      lv_obj_get_coords(lv_obj_get_child(scroller, n - 1), &a);
      int cb = a.y2 + 1 + (int)lv_obj_get_style_pad_bottom(scroller, LV_PART_MAIN);
      if (cb > bottom) bottom = cb;
    }
    scrollable = (lv_obj_get_scroll_bottom(scroller) > 0 || lv_obj_get_scroll_top(scroller) > 0) ? 1 : 0;
  }
  stlog("[UI] fs=%d page=%s content_bottom=%d scrollable=%d right=%d overflow=%d cols=%d",
        g_font_idx, name, bottom, scrollable, right, over, cols);
}

// ============================================================================
// Chat ring: strings live in PSRAM, fixed slots
// ============================================================================
#define SLOT_SZ (TXT_MAX + 1)
static char* g_ring = NULL;                  // CHAT_MAX * 2 * SLOT_SZ
static int   g_ring_seq[CHAT_MAX];
static int   g_ring_start = 0, g_ring_count = 0;
static int   g_next_seq = 0;                 // index of the next pair since boot / last clear

static inline char* ring_o(int slot) { return g_ring + (size_t)slot * 2 * SLOT_SZ; }
static inline char* ring_t(int slot) { return g_ring + (size_t)slot * 2 * SLOT_SZ + SLOT_SZ; }
static inline int   hist_cap(void)   { return clampi(g_settings.histPairs, 1, CHAT_MAX); }

static void ring_trim(int cap) {
  while (g_ring_count > cap) { g_ring_start = (g_ring_start + 1) % CHAT_MAX; g_ring_count--; }
}
static void ring_push(const char* o, const char* t, int seq) {
  if (!g_ring) return;
  ring_trim(hist_cap() - 1);
  int slot = (g_ring_start + g_ring_count) % CHAT_MAX;
  copy_utf8(ring_o(slot), SLOT_SZ, o);
  copy_utf8(ring_t(slot), SLOT_SZ, t);
  g_ring_seq[slot] = seq;
  g_ring_count++;
}

// ============================================================================
// Shared styles
// ============================================================================
static lv_style_t st_scr, st_sb, st_row, st_tile, st_card, st_line, st_pressed, st_press_tint;
extern "C" const uint8_t ttf_montserrat[];
extern "C" const size_t ttf_montserrat_len;
static lv_font_t* g_font84 = NULL;      // 84 px clock/splash font from the embedded TTF (falls back to 48 px)
static const lv_font_t* big_font(void) { return g_font84 ? g_font84 : &lv_font_montserrat_48; }
static lv_style_t st_f_body, st_f_title, st_f_card, st_f_small, st_f_clock;
static lv_style_t st_c_text, st_c_hint, st_c_sub, st_c_white, st_c_blue, st_c_red;
static lv_style_t st_bub_orig, st_bub_trans, st_sw_on, st_sbar, st_bar_main, st_bar_ind;
static lv_color_filter_dsc_t g_dsc_dark;

static lv_color_t darken_cb(const lv_color_filter_dsc_t* d, lv_color_t c, lv_opa_t o) {
  LV_UNUSED(d);
  return lv_color_darken(c, o);
}

static void styles_theme(void) {
  const Pal& p = pal();
  lv_style_set_bg_color(&st_scr, COL(p.bg));
  lv_style_set_bg_color(&st_sb, COL(p.sb));
  lv_style_set_border_color(&st_sb, COL(p.border));
  lv_style_set_bg_color(&st_row, COL(p.row));
  lv_style_set_border_color(&st_row, COL(p.border));
  lv_style_set_bg_color(&st_tile, COL(p.sb));
  lv_style_set_border_color(&st_tile, COL(p.border));
  lv_style_set_bg_color(&st_card, COL(p.card));
  lv_style_set_bg_color(&st_line, COL(p.border));
  lv_style_set_bg_color(&st_bub_trans, COL(p.card));
  lv_style_set_text_color(&st_c_text, COL(p.text));
  lv_style_set_text_color(&st_c_hint, COL(p.hint));
}

static void styles_font(void) {
  int i = clampi(g_font_idx, 1, 4) - 1;
  g_fbody = FONT_BODY[i];
  lv_style_set_text_font(&st_f_body,  FONT_BODY[i]);
  lv_style_set_text_font(&st_f_title, FONT_TITLE[i]);
  lv_style_set_text_font(&st_f_card,  FONT_CARD[i]);
  lv_style_set_text_font(&st_f_small, FONT_SMALL[i]);
}

static void styles_init(void) {
  lv_style_init(&st_scr);
  lv_style_set_bg_opa(&st_scr, LV_OPA_COVER);

  lv_style_init(&st_sb);
  lv_style_set_bg_opa(&st_sb, LV_OPA_COVER);
  lv_style_set_border_width(&st_sb, 1);

  lv_style_init(&st_row);
  lv_style_set_bg_opa(&st_row, LV_OPA_COVER);
  lv_style_set_border_width(&st_row, 1);
  lv_style_set_radius(&st_row, 10);

  lv_style_init(&st_tile);
  lv_style_set_bg_opa(&st_tile, LV_OPA_COVER);
  lv_style_set_border_width(&st_tile, 1);
  lv_style_set_radius(&st_tile, 14);

  lv_style_init(&st_card);
  lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
  lv_style_set_radius(&st_card, 10);

  lv_style_init(&st_line);
  lv_style_set_bg_opa(&st_line, LV_OPA_COVER);

  lv_color_filter_dsc_init(&g_dsc_dark, darken_cb);
  lv_style_init(&st_pressed);
  lv_style_set_color_filter_dsc(&st_pressed, &g_dsc_dark);
  lv_style_set_color_filter_opa(&st_pressed, LV_OPA_30);

  lv_style_init(&st_press_tint);
  lv_style_set_bg_color(&st_press_tint, COL(COL_BLUE));
  lv_style_set_bg_opa(&st_press_tint, LV_OPA_30);

  lv_style_init(&st_f_body);
  lv_style_init(&st_f_title);
  lv_style_init(&st_f_card);
  lv_style_init(&st_f_small);
  lv_style_init(&st_f_clock);
  lv_style_set_text_font(&st_f_clock, &lv_font_montserrat_48);

  lv_style_init(&st_c_text);
  lv_style_init(&st_c_hint);
  lv_style_init(&st_c_sub);   lv_style_set_text_color(&st_c_sub,   COL(COL_SUBTLE));
  lv_style_init(&st_c_white); lv_style_set_text_color(&st_c_white, lv_color_white());
  lv_style_init(&st_c_blue);  lv_style_set_text_color(&st_c_blue,  COL(COL_BLUE));
  lv_style_init(&st_c_red);   lv_style_set_text_color(&st_c_red,   COL(COL_RED));

  lv_style_init(&st_bub_orig);
  lv_style_set_bg_color(&st_bub_orig, COL(COL_BLUE));
  lv_style_set_bg_opa(&st_bub_orig, LV_OPA_COVER);
  lv_style_set_radius(&st_bub_orig, 12);
  lv_style_set_pad_hor(&st_bub_orig, 10);
  lv_style_set_pad_ver(&st_bub_orig, 5);

  lv_style_init(&st_bub_trans);
  lv_style_set_bg_opa(&st_bub_trans, LV_OPA_COVER);
  lv_style_set_radius(&st_bub_trans, 12);
  lv_style_set_pad_hor(&st_bub_trans, 10);
  lv_style_set_pad_ver(&st_bub_trans, 5);

  lv_style_init(&st_sw_on);
  lv_style_set_bg_color(&st_sw_on, COL(COL_BLUE));

  lv_style_init(&st_sbar);
  lv_style_set_bg_color(&st_sbar, COL(COL_SUBTLE));
  lv_style_set_bg_opa(&st_sbar, LV_OPA_70);
  lv_style_set_width(&st_sbar, 4);
  lv_style_set_radius(&st_sbar, LV_RADIUS_CIRCLE);

  lv_style_init(&st_bar_main);
  lv_style_set_bg_color(&st_bar_main, lv_color_white());
  lv_style_set_bg_opa(&st_bar_main, LV_OPA_40);
  lv_style_set_radius(&st_bar_main, LV_RADIUS_CIRCLE);
  lv_style_init(&st_bar_ind);
  lv_style_set_bg_color(&st_bar_ind, lv_color_white());
  lv_style_set_bg_opa(&st_bar_ind, LV_OPA_COVER);
  lv_style_set_radius(&st_bar_ind, LV_RADIUS_CIRCLE);

  styles_theme();
  styles_font();
}

static void theme_set(bool night) {
  g_night = night;
  styles_theme();
  lv_obj_report_style_change(NULL);          // refresh every object that uses a shared style
}

static void font_set(int idx) {
  g_font_idx = clampi(idx, 1, 4);
  styles_font();
  lv_obj_report_style_change(NULL);
}

// ============================================================================
// Widget construction helpers
// ============================================================================
static lv_obj_t* mk_box(lv_obj_t* p, int32_t w, int32_t h) {      // inert container
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, OF(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_size(o, w, h);
  return o;
}

static lv_obj_t* mk_btn(lv_obj_t* p, int32_t w, int32_t h, lv_event_cb_t cb, void* ud, lv_style_t* press) {
  lv_obj_t* o = mk_box(p, w, h);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_style(o, press ? press : &st_pressed, LV_STATE_PRESSED);
  if (cb) lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, ud);
  return o;
}

static lv_obj_t* mk_label(lv_obj_t* p, const char* txt, lv_style_t* font, lv_style_t* col) {
  lv_obj_t* l = lv_label_create(p);
  if (font) lv_obj_add_style(l, font, 0);
  if (col)  lv_obj_add_style(l, col, 0);
  lv_label_set_text(l, txt);
  return l;
}

static lv_obj_t* mk_scroller(lv_obj_t* p, int x, int y, int w, int h) {   // vertical flex column, scrollable
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, OF(LV_OBJ_FLAG_SCROLL_ELASTIC));
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_scroll_dir(o, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_add_style(o, &st_sbar, LV_PART_SCROLLBAR);
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(o, 6, 0);
  lv_obj_set_style_pad_row(o, 6, 0);
  return o;
}

static lv_obj_t* new_screen(void) {
  lv_obj_t* s = lv_obj_create(NULL);
  lv_obj_remove_style_all(s);
  lv_obj_add_style(s, &st_scr, 0);
  lv_obj_remove_flag(s, OF(LV_OBJ_FLAG_SCROLLABLE));
  return s;
}

// ============================================================================
// Screens
// ============================================================================
typedef enum { SC_NONE, SC_SPLASH, SC_HOME, SC_LANG, SC_TALK, SC_SETTINGS, SC_CONN, SC_DISP, SC_AUDIO, SC_ADV } ScrId;
static ScrId     g_cur = SC_NONE;
static ScrId     g_lang_from = SC_HOME;      // screen to return to from Languages
static lv_obj_t* g_scr = NULL;               // the one live screen object
static void goto_scr(ScrId id);

// Widgets of the live screen (zeroed on every screen switch)
static struct {
  lv_obj_t *clock, *wifi, *ssid, *batt_ic, *batt_tx;                 // Home
  lv_obj_t *chat, *hint, *rec, *rec_icon, *rec_bar, *sw_a, *sw_b;    // Talk
  lv_obj_t *conn_status;                                              // Settings > Connectivity
  lv_obj_t *sw_night;                                                 // Settings > Display (self-test drives it)
  lv_obj_t *tab[2], *tab_lbl[2], *card[LANG_COUNT], *card_lbl[LANG_COUNT];   // Languages
} W;

// ---- clock ------------------------------------------------------------------
static lv_obj_t* g_saver = NULL;
static lv_obj_t* g_saver_clock = NULL;

static void clock_text(char* buf, size_t n) {
  time_t t = time(NULL);
  if (t < 1600000000) { snprintf(buf, n, "--:--"); return; }       // time not synced yet
  struct tm tmv;
  localtime_r(&t, &tmv);
  snprintf(buf, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

static void clock_apply(void) {
  char b[8];
  clock_text(b, sizeof b);
  set_text(W.clock, b);
  if (g_saver_on) set_text(g_saver_clock, b);
}

// ---- status (Home status column + Connectivity status line) -----------------
static void status_apply(void) {
  if (W.wifi) {
    uint32_t col = COL_SUBTLE;
    if (g_st_wifi) {
      switch (g_st_q) {
        case NETQ_POOR: col = COL_RED;    break;
        case NETQ_OK:   col = COL_ORANGE; break;
        case NETQ_GOOD: col = COL_GREEN;  break;
        default:        col = COL_SUBTLE; break;
      }
    }
    lv_obj_set_style_text_color(W.wifi, COL(col), 0);
  }
  const char* ss = (g_st_wifi && g_st_ssid[0]) ? g_st_ssid : "Not connected";
  set_text(W.ssid, ss);
  set_text(W.conn_status, ss);

  if (W.batt_ic) {
    const char* ic;
    if      (g_st_batt > 87) ic = LV_SYMBOL_BATTERY_FULL;
    else if (g_st_batt > 62) ic = LV_SYMBOL_BATTERY_3;
    else if (g_st_batt > 37) ic = LV_SYMBOL_BATTERY_2;
    else if (g_st_batt > 12) ic = LV_SYMBOL_BATTERY_1;
    else                     ic = LV_SYMBOL_BATTERY_EMPTY;
    set_text(W.batt_ic, ic);
    bool low = (g_st_batt >= 0 && g_st_batt <= 12);
    lv_obj_set_style_text_color(W.batt_ic, COL(low ? COL_RED : pal().text), 0);
    char b[8];
    if (g_st_batt < 0) snprintf(b, sizeof b, "--%%");
    else               snprintf(b, sizeof b, "%d%%", g_st_batt > 100 ? 100 : g_st_batt);
    set_text(W.batt_tx, b);
  }
}

// ---- REC button state ---------------------------------------------------------
static void prog_apply(void) {
  if (!W.rec_bar) return;
  if (g_state == UI_RECORDING && g_prog_mx > 0) {
    lv_bar_set_range(W.rec_bar, 0, g_prog_mx);
    lv_bar_set_value(W.rec_bar, g_prog_el > g_prog_mx ? g_prog_mx : g_prog_el, LV_ANIM_OFF);
    lv_obj_remove_flag(W.rec_bar, OF(LV_OBJ_FLAG_HIDDEN));
  } else {
    lv_obj_add_flag(W.rec_bar, LV_OBJ_FLAG_HIDDEN);
  }
}

static void rec_apply(void) {
  if (!W.rec) return;
  uint32_t col;
  const char* ic;
  switch (g_state) {
    case UI_RECORDING:  col = COL_RED;    ic = LV_SYMBOL_AUDIO;      break;
    case UI_PROCESSING: col = COL_ORANGE; ic = LV_SYMBOL_REFRESH;    break;
    case UI_SPEAKING:   col = COL_GREEN;  ic = LV_SYMBOL_VOLUME_MAX; break;
    default:            col = COL_BLUE;   ic = LV_SYMBOL_AUDIO;      break;
  }
  lv_obj_set_style_bg_color(W.rec, COL(col), 0);
  set_text(W.rec_icon, ic);
  prog_apply();
}

// ---- chat ---------------------------------------------------------------------
static void hint_apply(void) {
  if (!W.hint) return;
  bool empty = (!W.chat || lv_obj_get_child_count(W.chat) == 0);
  if (empty) lv_obj_remove_flag(W.hint, OF(LV_OBJ_FLAG_HIDDEN));
  else       lv_obj_add_flag(W.hint, LV_OBJ_FLAG_HIDDEN);
}

static void pair_cb(lv_event_t* e) {
  app_on_bubble_tap((int)(intptr_t)lv_event_get_user_data(e));
}

static void bubble_make(lv_obj_t* pair, const char* txt, bool isOrig) {
  // orig bubble sits next to the REC button, translation on the far side
  bool right = isOrig ? !g_settings.leftHand : g_settings.leftHand;
  lv_obj_t* row = mk_box(pair, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, right ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  lv_obj_t* b = mk_box(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_add_style(b, isOrig ? &st_bub_orig : &st_bub_trans, 0);

  lv_obj_t* l = lv_label_create(b);
  lv_obj_add_style(l, &st_f_body, 0);
  lv_obj_add_style(l, isOrig ? &st_c_white : &st_c_text, 0);
  lv_label_set_text(l, txt);
  lv_point_t sz;
  lv_text_get_size(&sz, txt, g_fbody, 0, 0, BUB_TXT_MAX, LV_TEXT_FLAG_NONE);
  lv_obj_set_width(l, LV_MIN(sz.x + 2, BUB_TXT_MAX));    // explicit width: wraps reliably, bubble hugs the text
}

static void pair_make(int seq, const char* o, const char* t) {
  lv_obj_t* pair = mk_box(W.chat, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_add_flag(pair, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(pair, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(pair, 3, 0);
  lv_obj_add_event_cb(pair, pair_cb, LV_EVENT_CLICKED, (void*)(intptr_t)seq);
  if (o && o[0]) bubble_make(pair, o, true);
  if (t && t[0]) bubble_make(pair, t, false);
}

static void chat_trim_widgets(int keep) {
  if (!W.chat) return;
  while ((int)lv_obj_get_child_count(W.chat) > keep && lv_obj_get_child_count(W.chat) > 0)
    lv_obj_delete(lv_obj_get_child(W.chat, 0));
}

static void chat_add(const char* o, const char* t) {
  if ((!o || !o[0]) && (!t || !t[0])) return;
  int seq = g_next_seq++;
  ring_push(o, t, seq);
  if (!W.chat) return;                       // not on the Talk screen: bubbles are rebuilt from the ring on entry
  chat_trim_widgets(hist_cap() - 1);
  pair_make(seq, o, t);
  hint_apply();
  lv_obj_scroll_to_y(W.chat, LV_COORD_MAX, LV_ANIM_ON);
}

static void chat_clear(void) {
  g_ring_start = 0; g_ring_count = 0; g_next_seq = 0;
  if (W.chat) lv_obj_clean(W.chat);
  hint_apply();
}

static void chat_rebuild(void) {
  if (!W.chat || !g_ring) return;
  ring_trim(hist_cap());
  for (int i = 0; i < g_ring_count; i++) {
    int slot = (g_ring_start + i) % CHAT_MAX;
    pair_make(g_ring_seq[slot], ring_o(slot), ring_t(slot));
  }
  lv_obj_scroll_to_y(W.chat, LV_COORD_MAX, LV_ANIM_OFF);
}

// ============================================================================
// Toast (layer_top)
// ============================================================================
static lv_obj_t*   g_toast = NULL;
static lv_obj_t*   g_toast_lbl = NULL;
static lv_timer_t* g_toast_timer = NULL;

static void toast_hide(void) {
  if (g_toast) lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
  if (g_toast_timer) lv_timer_pause(g_toast_timer);
}
static void toast_timer_cb(lv_timer_t* t) { LV_UNUSED(t); toast_hide(); }
static void toast_click_cb(lv_event_t* e) { LV_UNUSED(e); toast_hide(); }

static void toast_init(void) {
  g_toast = mk_box(lv_layer_top(), LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_add_flag(g_toast, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_toast, 18, 0);
  lv_obj_set_style_pad_hor(g_toast, 16, 0);
  lv_obj_set_style_pad_ver(g_toast, 8, 0);
  lv_obj_align(g_toast, LV_ALIGN_TOP_MID, 0, 6);
  lv_obj_add_event_cb(g_toast, toast_click_cb, LV_EVENT_CLICKED, NULL);
  g_toast_lbl = lv_label_create(g_toast);
  lv_obj_set_style_text_color(g_toast_lbl, lv_color_white(), 0);
  lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
  g_toast_timer = lv_timer_create(toast_timer_cb, 3000, NULL);
  lv_timer_pause(g_toast_timer);
}

static void toast_show(ToastKind k, const char* text, uint32_t ms) {
  if (!g_toast || g_saver_on) return;
  uint32_t col = (k == TOAST_ERROR) ? COL_RED : (k == TOAST_WARN) ? COL_ORANGE : COL_BLUE;
  lv_obj_set_style_bg_color(g_toast, COL(col), 0);
  lv_obj_set_style_text_font(g_toast_lbl, g_fbody, 0);       // layer_top is not covered by report_style_change
  lv_label_set_text(g_toast_lbl, text);
  lv_point_t sz;
  lv_text_get_size(&sz, text, g_fbody, 0, 0, 480, LV_TEXT_FLAG_NONE);
  lv_obj_set_width(g_toast_lbl, sz.x + 2);
  lv_obj_remove_flag(g_toast, OF(LV_OBJ_FLAG_HIDDEN));
  lv_obj_move_foreground(g_toast);
  if (ms < 300) ms = 3000;
  lv_timer_set_period(g_toast_timer, ms);
  lv_timer_reset(g_toast_timer);
  lv_timer_resume(g_toast_timer);
}

// ============================================================================
// Confirm dialog (layer_top)
// ============================================================================
static lv_obj_t* g_dlg = NULL;
static void (*g_dlg_fn)(void) = NULL;

static void dlg_close(void) {
  if (g_dlg) { lv_obj_delete_async(g_dlg); g_dlg = NULL; }
  g_dlg_fn = NULL;
}
static void dlg_cancel_cb(lv_event_t* e) { LV_UNUSED(e); dlg_close(); }
static void dlg_ok_cb(lv_event_t* e) {
  LV_UNUSED(e);
  void (*fn)(void) = g_dlg_fn;
  dlg_close();
  if (fn) fn();
}

static void dlg_open(const char* msg, const char* okText, void (*fn)(void)) {
  if (g_dlg) return;
  const Pal& p = pal();
  g_dlg_fn = fn;

  g_dlg = lv_obj_create(lv_layer_top());                   // full-screen backdrop, swallows touches
  lv_obj_remove_style_all(g_dlg);
  lv_obj_remove_flag(g_dlg, OF(LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_size(g_dlg, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(g_dlg, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_dlg, LV_OPA_60, 0);

  lv_obj_t* box = mk_box(g_dlg, 400, LV_SIZE_CONTENT);
  lv_obj_center(box);
  lv_obj_set_style_bg_color(box, COL(p.row), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(box, COL(p.border), 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_radius(box, 14, 0);
  lv_obj_set_style_pad_all(box, 14, 0);
  lv_obj_set_style_pad_row(box, 14, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* l = lv_label_create(box);
  lv_obj_set_style_text_font(l, g_fbody, 0);
  lv_obj_set_style_text_color(l, COL(p.text), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, msg);
  lv_obj_set_width(l, lv_pct(100));

  lv_obj_t* row = mk_box(box, lv_pct(100), 44);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* bc = mk_btn(row, 150, 44, dlg_cancel_cb, NULL, NULL);
  lv_obj_set_style_bg_color(bc, COL(p.card), 0);
  lv_obj_set_style_bg_opa(bc, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bc, 12, 0);
  lv_obj_t* lc = lv_label_create(bc);
  lv_obj_set_style_text_font(lc, g_fbody, 0);
  lv_obj_set_style_text_color(lc, COL(p.text), 0);
  lv_label_set_text(lc, "Cancel");
  lv_obj_center(lc);

  lv_obj_t* bo = mk_btn(row, 150, 44, dlg_ok_cb, NULL, NULL);
  lv_obj_set_style_bg_color(bo, COL(COL_RED), 0);
  lv_obj_set_style_bg_opa(bo, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bo, 12, 0);
  lv_obj_t* lo = lv_label_create(bo);
  lv_obj_set_style_text_font(lo, g_fbody, 0);
  lv_obj_set_style_text_color(lo, lv_color_white(), 0);
  lv_label_set_text(lo, okText);
  lv_obj_center(lo);
}

// ============================================================================
// Screensaver overlay (layer_top, black + big clock). Not a screen: no rebuild on wake.
// ============================================================================
static void saver_hide(void) {
  g_saver_on = false;
  if (g_saver) lv_obj_add_flag(g_saver, LV_OBJ_FLAG_HIDDEN);
}
static void saver_show(void) {
  if (!g_saver) return;
  dlg_close();
  toast_hide();
  g_saver_on = true;
  clock_apply();
  lv_obj_remove_flag(g_saver, OF(LV_OBJ_FLAG_HIDDEN));
  lv_obj_move_foreground(g_saver);
}
static void saver_press_cb(lv_event_t* e) {
  LV_UNUSED(e);
  saver_hide();
  app_on_user_activity();
}
static void saver_init(void) {
  g_saver = mk_box(lv_layer_top(), SCR_W, SCR_H);
  lv_obj_add_flag(g_saver, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(g_saver, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(g_saver, LV_OPA_COVER, 0);
  g_saver_clock = lv_label_create(g_saver);
  lv_obj_set_style_text_font(g_saver_clock, big_font(), 0);
  lv_obj_set_style_text_color(g_saver_clock, lv_color_white(), 0);
  lv_label_set_text(g_saver_clock, "00:00");
  lv_obj_center(g_saver_clock);
  lv_obj_add_event_cb(g_saver, saver_press_cb, LV_EVENT_PRESSED, NULL);
  lv_obj_add_flag(g_saver, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// Settings glue
// ============================================================================
static int num_state(SettingId id) {
  switch (id) {
    case SET_VOLUME:        return g_settings.volume;
    case SET_MIC_GAIN:      return g_settings.micGain;
    case SET_AI_CREATIVITY: return g_settings.aiCreativity;
    case SET_BRIGHTNESS:    return g_settings.brightness;
    case SET_SCREEN_TIMEOUT:return g_settings.screenTimeoutS;
    case SET_FONT_SIZE:     return g_font_idx;
    case SET_TOUCH_SENS:    return g_settings.touchSens;
    case SET_HIST_PAIRS:    return g_settings.histPairs;
    case SET_RMS_THRESHOLD: return g_settings.rmsThreshold;
    default:                return 0;
  }
}

static bool sw_state(SettingId id) {
  switch (id) {
    case SET_NIGHT:         return g_night;
    case SET_NOISE:         return g_settings.noise;
    case SET_SAVE_HISTORY:  return g_settings.saveHistory;
    case SET_LEFT_HAND:     return g_settings.leftHand;
    case SET_FLIP_ENABLED:  return g_settings.flipEnabled;
    case SET_MIC_AGC:       return g_settings.micAGC;
    case SET_WIFI_ENABLED:  return g_settings.wifiEnabled;
    case SET_BLUETOOTH:     return g_bt;
    default:                return false;
  }
}

// UI-side effects that must not wait for the app to update g_settings
static void setting_local(SettingId id, int v) {
  switch (id) {
    case SET_NIGHT:      theme_set(v != 0); break;
    case SET_FONT_SIZE:  font_set(v); break;
    case SET_HIST_PAIRS: ring_trim(clampi(v, 1, CHAT_MAX)); break;
    case SET_BLUETOOTH:  g_bt = (v != 0); break;
    default: break;
  }
}

static void sw_cb(lv_event_t* e) {
  lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
  SettingId id = (SettingId)(intptr_t)lv_event_get_user_data(e);
  int v = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
  setting_local(id, v);
  app_on_setting_changed(id, v);
}

// ---- numeric rows: [-] value [+] ------------------------------------------------
enum { F_PLAIN, F_TIMEOUT, F_OFFZERO, F_TENTH };
typedef struct {
  SettingId id;
  int16_t lo, hi, step;
  const int16_t* list;                       // optional value table (index stepping)
  uint8_t nlist, fmt;
  int32_t val;
  lv_obj_t* lbl;
  lv_obj_t* bm;                              // [-] and [+] buttons (self-test sends real events to them)
  lv_obj_t* bp;
} NumRow;
static NumRow g_nr[8];
static int    g_nr_n = 0;

static const int16_t TIMEOUT_LIST[] = { 15, 30, 60, 120, 300, 0 };       // 0 = Never

static void nr_show(const NumRow* r) {
  char b[16];
  int v = r->val;
  switch (r->fmt) {
    case F_TIMEOUT:
      if (v == 0)      snprintf(b, sizeof b, "Never");
      else if (v < 60) snprintf(b, sizeof b, "%d s", v);
      else             snprintf(b, sizeof b, "%d min", v / 60);
      break;
    case F_OFFZERO:
      if (v == 0) snprintf(b, sizeof b, "Off"); else snprintf(b, sizeof b, "%d", v);
      break;
    case F_TENTH:
      snprintf(b, sizeof b, "%d.%d", v / 10, v % 10); break;
    default: snprintf(b, sizeof b, "%d", v); break;
  }
  set_text(r->lbl, b);
}

static int nr_list_idx(const NumRow* r) {      // exact match, else nearest ("Never" only matches 0)
  int best = 0, bd = 0x7fffffff;
  for (int i = 0; i < r->nlist; i++) {
    int v = r->list[i];
    int d = (v == r->val) ? 0 : ((v == 0 || r->val == 0) ? 100000 : abs(v - (int)r->val));
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

static void nr_cb(lv_event_t* e) {
  uintptr_t ud = (uintptr_t)lv_event_get_user_data(e);
  NumRow* r = &g_nr[ud >> 1];
  int dir = (ud & 1) ? 1 : -1;
  int nv;
  if (r->list) nv = r->list[clampi(nr_list_idx(r) + dir, 0, r->nlist - 1)];
  else         nv = clampi((int)r->val + dir * r->step, r->lo, r->hi);
  if (nv == r->val) return;
  r->val = nv;
  nr_show(r);
  setting_local(r->id, nv);
  app_on_setting_changed(r->id, nv);
}

// ---- row builders ----------------------------------------------------------------
static lv_obj_t* row_base(lv_obj_t* body, const char* label, const char* sub, lv_style_t* labelCol, lv_obj_t** subOut) {
  lv_obj_t* row = mk_box(body, lv_pct(100), ROW_H);
  lv_obj_add_style(row, &st_row, 0);
  lv_obj_set_style_pad_hor(row, 12, 0);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* left = mk_box(row, 0, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(left, 1);
  lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
  lv_obj_t* l = mk_label(left, label, &st_f_body, labelCol);
  lv_obj_set_width(l, lv_pct(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  if (sub) {
    lv_obj_t* s = mk_label(left, sub, &st_f_small, &st_c_sub);
    lv_obj_set_width(s, lv_pct(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
    if (subOut) *subOut = s;
  }
  return row;
}

static void step_btn(lv_obj_t* ctl, const char* sym, uintptr_t ud, lv_obj_t** out) {
  lv_obj_t* b = mk_btn(ctl, 52, 40, NULL, NULL, NULL);
  *out = b;
  lv_obj_set_ext_click_area(b, 3);                     // bigger touch area, same pixels
  lv_obj_add_style(b, &st_card, 0);
  lv_obj_add_event_cb(b, nr_cb, LV_EVENT_SHORT_CLICKED, (void*)ud);
  lv_obj_add_event_cb(b, nr_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void*)ud);
  lv_obj_t* l = mk_label(b, sym, &st_f_title, &st_c_text);
  lv_obj_center(l);
}

static void add_num(lv_obj_t* body, const char* label, SettingId id, int lo, int hi, int step,
                    uint8_t fmt, const int16_t* list, uint8_t nlist) {
  if (g_nr_n >= (int)(sizeof g_nr / sizeof g_nr[0])) return;
  int idx = g_nr_n++;
  NumRow* r = &g_nr[idx];
  r->id = id; r->lo = lo; r->hi = hi; r->step = step; r->list = list; r->nlist = nlist; r->fmt = fmt;
  r->val = clampi(num_state(id), lo, hi);
  if (list) { r->val = num_state(id); r->val = list[nr_list_idx(r)]; }

  lv_obj_t* row = row_base(body, label, NULL, &st_c_text, NULL);
  lv_obj_t* ctl = mk_box(row, LV_SIZE_CONTENT, 40);
  lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(ctl, 6, 0);
  step_btn(ctl, LV_SYMBOL_MINUS, ((uintptr_t)idx << 1), &r->bm);
  r->lbl = mk_label(ctl, "", &st_f_title, &st_c_text);
  lv_obj_set_width(r->lbl, 76);
  lv_obj_set_style_text_align(r->lbl, LV_TEXT_ALIGN_CENTER, 0);
  step_btn(ctl, LV_SYMBOL_PLUS, ((uintptr_t)idx << 1) | 1, &r->bp);
  nr_show(r);
}

static void add_sw(lv_obj_t* body, const char* label, const char* sub, SettingId id, lv_obj_t** subOut) {
  lv_obj_t* row = row_base(body, label, sub, &st_c_text, subOut);
  lv_obj_t* sw = lv_switch_create(row);
  lv_obj_set_size(sw, 56, 30);
  lv_obj_set_ext_click_area(sw, 8);                    // bigger touch area, same pixels
  lv_obj_add_style(sw, &st_sw_on, SEL((uint32_t)LV_PART_INDICATOR | (uint32_t)LV_STATE_CHECKED));
  if (sw_state(id)) lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(sw, sw_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)id);
  if (id == SET_NIGHT) W.sw_night = sw;
}

static void add_action(lv_obj_t* body, const char* label, const char* sub, lv_event_cb_t cb) {
  lv_obj_t* row = row_base(body, label, sub, &st_c_red, NULL);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_style(row, &st_pressed, LV_STATE_PRESSED);
  lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
}

// ---- confirm actions ----------------------------------------------------------------
static void do_wifi_reset(void)  { app_on_wifi_reset(); }
static void do_clear_history(void) {
  chat_clear();
  app_on_clear_history();
}
static void cb_wifi_reset(lv_event_t* e) { LV_UNUSED(e); dlg_open("Forget the WiFi network and reboot into setup?", "Reset", do_wifi_reset); }
static void cb_clear_hist(lv_event_t* e) { LV_UNUSED(e); dlg_open("Clear all conversation history?", "Clear", do_clear_history); }

// ============================================================================
// Sidebar / navigation widgets
// ============================================================================
static void cb_nav(lv_event_t* e) { goto_scr((ScrId)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t* mk_sidebar(lv_obj_t* s, int x, bool borderLeft) {
  lv_obj_t* sb = mk_box(s, SB_W, SCR_H);
  lv_obj_set_pos(sb, x, 0);
  lv_obj_add_style(sb, &st_sb, 0);
  lv_obj_set_style_border_side(sb, borderLeft ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_RIGHT, 0);
  return sb;
}

static void mk_nav(lv_obj_t* sb, int y, int h, const char* sym, const char* txt, lv_event_cb_t cb, ScrId target) {
  lv_obj_t* b = mk_btn(sb, SB_W - 1, h, cb, (void*)(intptr_t)target, &st_press_tint);
  lv_obj_set_pos(b, 0, y);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(b, 2, 0);
  lv_obj_t* i = mk_label(b, sym, NULL, &st_c_blue);
  lv_obj_set_style_text_font(i, &lv_font_montserrat_16, 0);
  lv_obj_t* t = mk_label(b, txt, NULL, &st_c_blue);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
}

// ============================================================================
// Screen builders
// ============================================================================
static void splash_done_cb(lv_timer_t* t) { LV_UNUSED(t); goto_scr(SC_HOME); }

static void build_splash(lv_obj_t* s) {                    // s = the display's default screen
  lv_obj_remove_style_all(s);
  lv_obj_set_style_bg_color(s, COL(0x0a0a0a), 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_remove_flag(s, OF(LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t* a = lv_label_create(s);
  lv_obj_set_style_text_font(a, big_font(), 0);
  lv_obj_set_style_text_color(a, lv_color_white(), 0);
  lv_label_set_text(a, "Translator");
  lv_obj_t* b = lv_label_create(s);
  lv_obj_set_style_text_font(b, big_font(), 0);
  lv_obj_set_style_text_color(b, COL(COL_BLUE), 0);
  lv_label_set_text(b, "OS");
}

static void home_tile(lv_obj_t* p, const char* sym, const char* txt, ScrId target) {
  lv_obj_t* b = mk_btn(p, 0, lv_pct(100), cb_nav, (void*)(intptr_t)target, NULL);
  lv_obj_set_flex_grow(b, 1);
  lv_obj_add_style(b, &st_tile, 0);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(b, 6, 0);
  lv_obj_t* i = mk_label(b, sym, NULL, &st_c_blue);
  lv_obj_set_style_text_font(i, &lv_font_montserrat_48, 0);
  mk_label(b, txt, &st_f_title, &st_c_text);
}

static lv_obj_t* build_home(void) {
  lv_obj_t* s = new_screen();

  lv_obj_t* info = mk_box(s, HOME_INFO_W, SCR_H);                   // clock / wifi / battery
  lv_obj_add_style(info, &st_sb, 0);
  lv_obj_set_style_border_side(info, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_pad_all(info, 12, 0);
  lv_obj_set_style_pad_row(info, 6, 0);
  lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  W.clock = mk_label(info, "00:00", &st_f_clock, &st_c_text);

  lv_obj_t* r1 = mk_box(info, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r1, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r1, 8, 0);
  W.wifi = mk_label(r1, LV_SYMBOL_WIFI, NULL, NULL);
  lv_obj_set_style_text_font(W.wifi, &lv_font_montserrat_20, 0);
  W.ssid = mk_label(r1, "Not connected", &st_f_card, &st_c_sub);
  lv_obj_set_flex_grow(W.ssid, 1);
  lv_label_set_long_mode(W.ssid, LV_LABEL_LONG_MODE_DOTS);

  lv_obj_t* r2 = mk_box(info, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r2, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r2, 8, 0);
  W.batt_ic = mk_label(r2, LV_SYMBOL_BATTERY_FULL, NULL, NULL);
  lv_obj_set_style_text_font(W.batt_ic, &lv_font_montserrat_20, 0);
  W.batt_tx = mk_label(r2, "--%", &st_f_body, &st_c_text);

  lv_obj_t* tiles = mk_box(s, SCR_W - HOME_INFO_W, SCR_H);
  lv_obj_set_pos(tiles, HOME_INFO_W, 0);
  lv_obj_set_style_pad_all(tiles, 8, 0);
  lv_obj_set_style_pad_column(tiles, 8, 0);
  lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
  home_tile(tiles, LV_SYMBOL_LIST,     "Languages", SC_LANG);
  home_tile(tiles, LV_SYMBOL_AUDIO,    "Talk",      SC_TALK);
  home_tile(tiles, LV_SYMBOL_SETTINGS, "Settings",  SC_SETTINGS);

  clock_apply();
  status_apply();
  return s;
}

// ---- Languages ----------------------------------------------------------------------
static int g_lang_edit = 0;                  // 0 = tab A, 1 = tab B
static int g_la = 0, g_lb = 1;               // selections shown (mirror of what we sent to the app)

static void lang_refresh(void) {
  const Pal& p = pal();
  int sel[2] = { g_la, g_lb };
  for (int t = 0; t < 2; t++) {
    if (!W.tab[t]) continue;
    bool act = (g_lang_edit == t);
    lv_obj_set_style_bg_color(W.tab[t], act ? COL(COL_BLUE) : COL(p.card), 0);
    lv_obj_set_style_text_color(W.tab_lbl[t], act ? lv_color_white() : COL(p.text), 0);
    set_text(W.tab_lbl[t], lang_name(sel[t]));
  }
  for (int i = 0; i < LANG_COUNT; i++) {
    if (!W.card[i]) continue;
    bool a = (i == g_la), b = (i == g_lb);
    uint32_t bg = a ? p.selA : b ? p.selB : p.bg;
    uint32_t bd = a ? COL_BLUE : b ? COL_GREEN : p.border;
    uint32_t tc = a ? COL_BLUE : b ? COL_GREEN : p.text;
    lv_obj_set_style_bg_color(W.card[i], COL(bg), 0);
    lv_obj_set_style_border_color(W.card[i], COL(bd), 0);
    lv_obj_set_style_text_color(W.card_lbl[i], COL(tc), 0);
  }
}

static void cb_lang_tab(lv_event_t* e) {
  g_lang_edit = (int)(intptr_t)lv_event_get_user_data(e);
  lang_refresh();
}
static void cb_lang_pick(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (g_lang_edit == 0) { g_la = idx; app_on_setting_changed(SET_LANG_A, idx); }
  else                  { g_lb = idx; app_on_setting_changed(SET_LANG_B, idx); }
  lang_refresh();
}
static void cb_lang_done(lv_event_t* e) { LV_UNUSED(e); goto_scr(g_lang_from); }

static lv_obj_t* build_lang(void) {
  lv_obj_t* s = new_screen();
  g_la = clampi(g_settings.langA, 0, LANG_COUNT - 1);
  g_lb = clampi(g_settings.langB, 0, LANG_COUNT - 1);
  g_lang_edit = 0;

  lv_obj_t* sb = mk_sidebar(s, 0, false);
  mk_nav(sb, 0, SCR_H, LV_SYMBOL_HOME, "Home", cb_nav, SC_HOME);

  const int tabY = 3, tabH = 36;
  for (int t = 0; t < 2; t++) {
    lv_obj_t* b = mk_btn(s, 150, tabH, cb_lang_tab, (void*)(intptr_t)t, NULL);
    lv_obj_set_pos(b, SB_W + 6 + t * 156, tabY);
    lv_obj_set_ext_click_area(b, 3);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 18, 0);
    W.tab[t] = b;
    W.tab_lbl[t] = mk_label(b, "", &st_f_title, NULL);
    lv_obj_set_width(W.tab_lbl[t], 130);
    lv_label_set_long_mode(W.tab_lbl[t], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(W.tab_lbl[t], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(W.tab_lbl[t]);
  }

  lv_obj_t* done = mk_btn(s, 104, tabH, cb_lang_done, NULL, NULL);
  lv_obj_set_pos(done, SCR_W - 104 - 6, tabY);
  lv_obj_set_ext_click_area(done, 3);
  lv_obj_set_style_bg_color(done, COL(COL_BLUE), 0);
  lv_obj_set_style_bg_opa(done, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(done, 18, 0);
  lv_obj_t* dl = mk_label(done, LV_SYMBOL_OK " Done", &st_f_title, &st_c_white);
  lv_obj_center(dl);

  lv_obj_t* sep = mk_box(s, CONTENT_W, 1);
  lv_obj_set_pos(sep, SB_W, tabY + tabH + 3);
  lv_obj_add_style(sep, &st_line, 0);

  const int gx = 4, gy = 4;
  const int top = tabY + tabH + 7;
  const int cw = (CONTENT_W - 6 * 2 - gx * 4) / 5;
  const int ch = (SCR_H - top - 4 - gy) / 2;
  for (int i = 0; i < LANG_COUNT; i++) {
    int col = i % 5, row = i / 5;
    lv_obj_t* c = mk_btn(s, cw, ch, cb_lang_pick, (void*)(intptr_t)i, NULL);
    lv_obj_set_pos(c, SB_W + 6 + col * (cw + gx), top + row * (ch + gy));
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_radius(c, 10, 0);
    W.card[i] = c;
    W.card_lbl[i] = mk_label(c, LANG_NAMES[i], &st_f_card, NULL);
    lv_obj_set_width(W.card_lbl[i], cw - 8);
    lv_label_set_long_mode(W.card_lbl[i], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(W.card_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(W.card_lbl[i]);
  }
  lang_refresh();
  return s;
}

// ---- Talk -----------------------------------------------------------------------------
static int g_shown_a = -1, g_shown_b = -1;

static void swap_labels_apply(void) {
  if (!W.sw_a) return;
  g_shown_a = g_settings.langA;
  g_shown_b = g_settings.langB;
  set_text(W.sw_a, lang_name(g_shown_a));
  set_text(W.sw_b, lang_name(g_shown_b));
}

static void cb_swap(lv_event_t* e) { LV_UNUSED(e); app_on_swap_langs(); }
static void cb_rec(lv_event_t* e)  { LV_UNUSED(e); app_on_rec_tap(); }

static lv_obj_t* build_talk(void) {
  lv_obj_t* s = new_screen();
  bool lh = g_settings.leftHand;
  int sbx   = lh ? SCR_W - SB_W : 0;
  int recx  = lh ? 0 : SCR_W - REC_W;
  int chatx = lh ? REC_W : SB_W;

  lv_obj_t* sb = mk_sidebar(s, sbx, lh);
  mk_nav(sb, 0, SCR_H, LV_SYMBOL_HOME, "Home", cb_nav, SC_HOME);

  // language swap pill (strip above the chat)
  lv_obj_t* pill = mk_btn(s, LV_SIZE_CONTENT, 26, cb_swap, NULL, NULL);
  lv_obj_set_pos(pill, chatx + 8, 2);
  lv_obj_set_ext_click_area(pill, 6);                  // chat is created later and wins any overlap
  lv_obj_add_style(pill, &st_tile, 0);
  lv_obj_set_style_radius(pill, 13, 0);
  lv_obj_set_style_pad_hor(pill, 12, 0);
  lv_obj_set_style_pad_column(pill, 8, 0);
  lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  W.sw_a = mk_label(pill, "", &st_f_card, &st_c_text);
  lv_obj_t* arr = mk_label(pill, LV_SYMBOL_SHUFFLE, &st_f_card, &st_c_sub);
  LV_UNUSED(arr);
  W.sw_b = mk_label(pill, "", &st_f_card, &st_c_text);
  swap_labels_apply();

  // chat
  W.chat = mk_scroller(s, chatx, STRIP_H, CHAT_W, SCR_H - STRIP_H);
  lv_obj_set_scrollbar_mode(W.chat, LV_SCROLLBAR_MODE_ACTIVE);

  W.hint = mk_label(s, "Press REC to speak  " LV_SYMBOL_BULLET "  press again to stop", &st_f_card, &st_c_hint);
  lv_obj_set_width(W.hint, CHAT_W - 24);
  lv_label_set_long_mode(W.hint, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_align(W.hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(W.hint, LV_ALIGN_TOP_LEFT, chatx + 12, STRIP_H + 52);

  // REC button column
  W.rec = mk_btn(s, REC_W - 6, SCR_H - 12, cb_rec, NULL, NULL);
  lv_obj_set_pos(W.rec, recx + (lh ? 6 : 0), 6);
  lv_obj_set_style_bg_opa(W.rec, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(W.rec, 10, 0);
  W.rec_icon = mk_label(W.rec, LV_SYMBOL_AUDIO, NULL, &st_c_white);
  lv_obj_set_style_text_font(W.rec_icon, &lv_font_montserrat_28, 0);
  lv_obj_center(W.rec_icon);
  W.rec_bar = lv_bar_create(W.rec);
  lv_obj_remove_style_all(W.rec_bar);
  lv_obj_add_style(W.rec_bar, &st_bar_main, LV_PART_MAIN);
  lv_obj_add_style(W.rec_bar, &st_bar_ind, LV_PART_INDICATOR);
  lv_obj_remove_flag(W.rec_bar, OF(LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_size(W.rec_bar, 34, 6);
  lv_obj_align(W.rec_bar, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_add_flag(W.rec_bar, LV_OBJ_FLAG_HIDDEN);

  chat_rebuild();
  hint_apply();
  rec_apply();
  return s;
}

// ---- Settings menu ---------------------------------------------------------------------
static void cat_tile(lv_obj_t* p, const char* sym, const char* txt, ScrId target) {
  lv_obj_t* b = mk_btn(p, 0, lv_pct(100), cb_nav, (void*)(intptr_t)target, NULL);
  lv_obj_set_flex_grow(b, 1);
  lv_obj_add_style(b, &st_tile, 0);
  lv_obj_set_style_pad_all(b, 6, 0);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(b, 6, 0);
  lv_obj_t* i = mk_label(b, sym, NULL, &st_c_blue);
  lv_obj_set_style_text_font(i, &lv_font_montserrat_28, 0);
  lv_obj_t* t = mk_label(b, txt, &st_f_title, &st_c_text);
  lv_obj_set_width(t, lv_pct(100));
  lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
}

static lv_obj_t* build_settings(void) {
  lv_obj_t* s = new_screen();
  lv_obj_t* sb = mk_sidebar(s, 0, false);
  mk_nav(sb, 0, SCR_H, LV_SYMBOL_HOME, "Home", cb_nav, SC_HOME);

  lv_obj_t* tiles = mk_box(s, CONTENT_W, SCR_H);
  lv_obj_set_pos(tiles, SB_W, 0);
  lv_obj_set_style_pad_all(tiles, 8, 0);
  lv_obj_set_style_pad_column(tiles, 8, 0);
  lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
  cat_tile(tiles, LV_SYMBOL_WIFI,       "Connect",        SC_CONN);
  cat_tile(tiles, LV_SYMBOL_IMAGE,      "Display & Text", SC_DISP);
  cat_tile(tiles, LV_SYMBOL_VOLUME_MAX, "Audio & Touch",  SC_AUDIO);
  cat_tile(tiles, LV_SYMBOL_SETTINGS,   "Advanced",       SC_ADV);
  return s;
}

// ---- Settings sub screens --------------------------------------------------------------
static lv_obj_t* sub_begin(const char* title, lv_obj_t** bodyOut) {
  g_nr_n = 0;
  lv_obj_t* s = new_screen();

  lv_obj_t* sb = mk_sidebar(s, 0, false);
  mk_nav(sb, 0, SCR_H / 2, LV_SYMBOL_LEFT, "Back", cb_nav, SC_SETTINGS);
  mk_nav(sb, SCR_H / 2, SCR_H / 2, LV_SYMBOL_HOME, "Home", cb_nav, SC_HOME);
  lv_obj_t* ln = mk_box(sb, SB_W - 12, 1);
  lv_obj_set_pos(ln, 5, SCR_H / 2);
  lv_obj_add_style(ln, &st_line, 0);

  lv_obj_t* tb = mk_box(s, CONTENT_W, TITLE_H);
  lv_obj_set_pos(tb, SB_W, 0);
  lv_obj_add_style(tb, &st_sb, 0);
  lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_t* tl = mk_label(tb, title, &st_f_title, &st_c_text);
  lv_obj_align(tl, LV_ALIGN_LEFT_MID, 12, 0);

  *bodyOut = mk_scroller(s, SB_W, TITLE_H, CONTENT_W, SCR_H - TITLE_H);
  return s;
}

static lv_obj_t* build_conn(void) {
  lv_obj_t* body;
  lv_obj_t* s = sub_begin("Connect", &body);
  add_sw(body, "WiFi", "Not connected", SET_WIFI_ENABLED, &W.conn_status);
  add_sw(body, "Bluetooth", NULL, SET_BLUETOOTH, NULL);
  add_action(body, "Reset WiFi", "Forget network, reboot to setup", cb_wifi_reset);
  status_apply();
  return s;
}

static lv_obj_t* build_disp(void) {
  lv_obj_t* body;
  lv_obj_t* s = sub_begin("Display & Text", &body);
  add_num(body, "Brightness", SET_BRIGHTNESS, 1, 10, 1, F_PLAIN, NULL, 0);
  add_num(body, "Screen Timeout", SET_SCREEN_TIMEOUT, 0, 300, 0, F_TIMEOUT, TIMEOUT_LIST, (uint8_t)(sizeof TIMEOUT_LIST / sizeof TIMEOUT_LIST[0]));
  add_num(body, "Font Size", SET_FONT_SIZE, 1, 4, 1, F_PLAIN, NULL, 0);
  add_sw(body, "Night Mode", "Dark theme", SET_NIGHT, NULL);
  add_sw(body, "Flip for others", "Share Screen", SET_FLIP_ENABLED, NULL);
  return s;
}

static lv_obj_t* build_audio(void) {
  lv_obj_t* body;
  lv_obj_t* s = sub_begin("Audio & Touch", &body);
  add_num(body, "Volume", SET_VOLUME, 1, 10, 1, F_PLAIN, NULL, 0);
  add_num(body, "Mic Gain", SET_MIC_GAIN, 1, 8, 1, F_PLAIN, NULL, 0);
  add_num(body, "Touch Sensitivity", SET_TOUCH_SENS, 1, 10, 1, F_PLAIN, NULL, 0);
  add_sw(body, "Mic AGC", NULL, SET_MIC_AGC, NULL);
  add_sw(body, "Noise Filter", NULL, SET_NOISE, NULL);
  return s;
}

static lv_obj_t* build_adv(void) {
  lv_obj_t* body;
  lv_obj_t* s = sub_begin("Advanced", &body);
  add_num(body, "AI Creativity", SET_AI_CREATIVITY, 0, 10, 1, F_TENTH, NULL, 0);
  add_num(body, "RMS Threshold", SET_RMS_THRESHOLD, 0, 3000, 100, F_OFFZERO, NULL, 0);
  add_sw(body, "Save History", NULL, SET_SAVE_HISTORY, NULL);
  add_num(body, "Pairs to save", SET_HIST_PAIRS, 1, CHAT_MAX, 1, F_PLAIN, NULL, 0);
  add_sw(body, "Left-hand Mode", NULL, SET_LEFT_HAND, NULL);
  add_action(body, "Clear History", NULL, cb_clear_hist);
  return s;
}

// ============================================================================
// Screen switching: build new, load, delete old (exactly one screen alive)
// ============================================================================
static void goto_scr(ScrId id) {
  if (id == SC_LANG && g_cur != SC_LANG) g_lang_from = g_cur;
  memset(&W, 0, sizeof W);
  lv_obj_t* old = g_scr;
  lv_obj_t* s = NULL;
  switch (id) {
    case SC_HOME:     s = build_home();     break;
    case SC_LANG:     s = build_lang();     break;
    case SC_TALK:     s = build_talk();     break;
    case SC_SETTINGS: s = build_settings(); break;
    case SC_CONN:     s = build_conn();     break;
    case SC_DISP:     s = build_disp();     break;
    case SC_AUDIO:    s = build_audio();    break;
    case SC_ADV:      s = build_adv();      break;
    default:          s = build_home(); id = SC_HOME; break;
  }
  g_scr = s;
  g_cur = id;
  lv_screen_load(s);
  if (old && old != s) lv_obj_delete(old);
}

// ============================================================================
// Queue drain (LVGL task, every 15 ms) and periodic timers
// ============================================================================
static void apply_msg(const UiMsg* m) {
  switch (m->type) {
    case UM_STATE:
      g_state = (UiState)m->u.st.state;
      if (g_state != UI_RECORDING) { g_prog_el = 0; g_prog_mx = 0; }
      rec_apply();
      break;
    case UM_BUBBLE:
      chat_add(m->u.bub.orig, m->u.bub.trans);
      break;
    case UM_TOAST:
      toast_show((ToastKind)m->u.toast.kind, m->u.toast.text, m->u.toast.ms);
      break;
    case UM_STATUS:
      g_st_wifi = m->u.status.wifi != 0;
      g_st_batt = m->u.status.batt;
      g_st_q    = (NetQuality)m->u.status.q;
      strncpy(g_st_ssid, m->u.status.ssid, sizeof g_st_ssid - 1);
      g_st_ssid[sizeof g_st_ssid - 1] = 0;
      status_apply();
      break;
    case UM_PROGRESS:
      g_prog_el = m->u.prog.el;
      g_prog_mx = m->u.prog.mx;
      prog_apply();
      break;
    case UM_SAVER:
      if (m->u.flag.on) saver_show(); else saver_hide();
      break;
    case UM_CLEAR:
      chat_clear();
      break;
    case UM_FLIP:
      lvgl_port_set_flipped(m->u.flag.on != 0);
      break;
    default: break;
  }
}

static void drain_cb(lv_timer_t* t) {
  LV_UNUSED(t);
  // the app swaps languages itself (g_settings), follow it
  if (W.sw_a && (g_settings.langA != g_shown_a || g_settings.langB != g_shown_b)) swap_labels_apply();

  if (!g_q) return;
  for (int n = 0; n < 6 && xQueueReceive(g_q, &s_msg, 0) == pdTRUE; n++) {
    apply_msg(&s_msg);
    if (s_msg.type == UM_BUBBLE) break;      // at most one bubble build per tick
  }
}

static void clock_cb(lv_timer_t* t) { LV_UNUSED(t); clock_apply(); }

static void indev_press_cb(lv_event_t* e) { LV_UNUSED(e); app_on_user_activity(); }

// ============================================================================
// Public API
// ============================================================================
extern "C" {

void ui_init(void) {
  g_q = xQueueCreateWithCaps(Q_LEN, sizeof(UiMsg), MALLOC_CAP_SPIRAM);
  if (!g_q) ESP_LOGE(TAG, "queue alloc failed, ui_post_* will be dropped");

  g_ring = (char*)heap_caps_malloc((size_t)CHAT_MAX * 2 * SLOT_SZ, MALLOC_CAP_SPIRAM);
  if (!g_ring) ESP_LOGE(TAG, "chat ring alloc failed, history disabled");

  g_night = g_settings.night;
  g_font_idx = clampi(g_settings.fontSize, 1, 4);
  g_la = clampi(g_settings.langA, 0, LANG_COUNT - 1);
  g_lb = clampi(g_settings.langB, 0, LANG_COUNT - 1);
  styles_init();

  g_scr = lv_screen_active();                // the display's default screen doubles as the splash
  g_font84 = lv_tiny_ttf_create_data(ttf_montserrat, ttf_montserrat_len, 84);
  Serial.printf("[UI] 84 px font %s\n", g_font84 ? "ok" : "FAILED (using 48 px)");
  g_cur = SC_SPLASH;
  build_splash(g_scr);

  toast_init();
  saver_init();

  for (lv_indev_t* in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
    lv_indev_add_event_cb(in, indev_press_cb, LV_EVENT_PRESSED, NULL);
  for (lv_indev_t* in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
    lv_indev_add_event_cb(in, indev_press_cb, LV_EVENT_PRESSING, NULL);   // held / dragging / slider: keep the idle timer reset
    lv_indev_add_event_cb(in, indev_press_cb, LV_EVENT_RELEASED, NULL);
    lv_indev_add_event_cb(in, indev_press_cb, LV_EVENT_SCROLL_BEGIN, NULL);
  }

  lv_timer_create(drain_cb, 15, NULL);
  lv_timer_create(clock_cb, 5000, NULL);
  lv_timer_t* sp = lv_timer_create(splash_done_cb, 2000, NULL);
  lv_timer_set_repeat_count(sp, 1);
}

void ui_post_state(UiState s) {
  if (!g_q) return;
  UiMsg m; m.type = UM_STATE; m.u.st.state = (uint8_t)s;
  xQueueSend(g_q, &m, 0);
}

void ui_post_bubble(const char* orig, const char* trans) {
  if (!g_q) return;
  UiMsg m; m.type = UM_BUBBLE;
  copy_utf8(m.u.bub.orig,  sizeof m.u.bub.orig,  orig);
  copy_utf8(m.u.bub.trans, sizeof m.u.bub.trans, trans);
  xQueueSend(g_q, &m, 0);
}

void ui_post_toast(ToastKind k, const char* text, uint32_t ms) {
  if (!g_q) return;
  UiMsg m; m.type = UM_TOAST; m.u.toast.kind = (uint8_t)k; m.u.toast.ms = ms;
  copy_utf8(m.u.toast.text, sizeof m.u.toast.text, text);
  xQueueSend(g_q, &m, 0);
}

void ui_post_status(bool wifi, int rssi, int battPct, NetQuality q, const char* ssid) {
  if (!g_q) return;
  UiMsg m; m.type = UM_STATUS;
  m.u.status.wifi = wifi ? 1 : 0;
  m.u.status.rssi = (int16_t)rssi;
  m.u.status.batt = (int16_t)battPct;
  m.u.status.q = (uint8_t)q;
  copy_utf8(m.u.status.ssid, sizeof m.u.status.ssid, ssid);
  xQueueSend(g_q, &m, 0);
}

void ui_post_rec_progress(uint16_t ms_elapsed, uint16_t ms_max) {
  if (!g_q) return;
  UiMsg m; m.type = UM_PROGRESS; m.u.prog.el = ms_elapsed; m.u.prog.mx = ms_max;
  xQueueSend(g_q, &m, 0);
}

void ui_post_screensaver(bool on) {
  if (!g_q) return;
  UiMsg m; m.type = UM_SAVER; m.u.flag.on = on ? 1 : 0;
  xQueueSend(g_q, &m, 0);
}

void ui_post_clear_history(void) {
  if (!g_q) return;
  UiMsg m; m.type = UM_CLEAR;
  xQueueSend(g_q, &m, 0);
}

void ui_post_flip(bool flipped) {
  if (!g_q) return;
  UiMsg m; m.type = UM_FLIP; m.u.flag.on = flipped ? 1 : 0;
  xQueueSend(g_q, &m, 0);
}

}  // extern "C"
