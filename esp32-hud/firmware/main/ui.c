/* A.L.I.S.H.A. HUD — LVGL 9 UI for the board's 320x240 screen.
 * A.L.I.S.H.A. = Advanced Learning Intelligent System for Human Assistance.
 *
 * Layout: a 22 px status bar (name, page dots, mic, link, clock) above five pages
 * that the UP/DOWN buttons flip through:
 *   CORE  voice console: reactor orb with the live mic level, state tag, voice bars,
 *         and two cards with what you said and what Alisha answered
 *   SYS   CPU / temperature / memory gauges and history
 *   NET   host, address, traffic rates and history
 *   OPS   security / activity feed from the Pi's journal
 *   KEYS  what each button does
 * The DEEP DIVE screensaver (saver.c) takes over after 3 min idle.
 *
 * Text uses Hack (the same family as the desktop A.L.I.S.H.A. panel), converted to LVGL
 * fonts by scripts/make_lvgl_font.py. Every text box has a fixed size and either clips
 * or ends in "..." so nothing can spill over another element. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_lvgl_port.h"
#include "ui.h"
#include "saver.h"

#define W 320
#define H 240
#define BAR_H 22
#define PAGE_H (H - BAR_H)
#define NPAGES 5

/* palette: deep navy background, cyan accent, soft status colours */
#define C_BG     0x03070B
#define C_PANEL  0x08121A
#define C_EDGE   0x15303A
#define C_DIM    0x102A33
#define C_CYAN   0x22D3EE
#define C_GREEN  0x34D399
#define C_AMBER  0xFBBF24
#define C_RED    0xF43F5E
#define C_TEXT   0xE2F3F7
#define C_MUTED  0x5F7F88
#define C_WHITE  0xF0FBFF
#define C_VIOLET 0xA78BFA
#define C_ACC    0x1B9BB3   /* corner brackets on ordinary panels */
#define C_EDGE2  0x2C5866   /* faint labels such as CH.01 */

/* fonts (font_hack_*.c) */
LV_FONT_DECLARE(font_hack_10)
LV_FONT_DECLARE(font_hack_12)
LV_FONT_DECLARE(font_hack_bold_14)
LV_FONT_DECLARE(font_hack_bold_20)
LV_FONT_DECLARE(font_hack_bold_34)
LV_FONT_DECLARE(font_icons_13)
#define F_CAP   (&font_hack_10)          /* captions        */
#define F_BODY  (&font_hack_12)          /* body text       */
#define F_HEAD  (&font_hack_bold_14)     /* headings, keys  */
#define F_BIG   (&font_hack_bold_20)     /* gauge values    */
#define F_CORE  (&font_hack_bold_34)     /* the "A" in the orb */
#define F_ICON  (&font_icons_13)         /* Font Awesome mic / plug icons */
#define ICON_MIC     "\xEF\x84\xB0"     /* U+F130 microphone       */
#define ICON_MIC_OFF "\xEF\x84\xB1"     /* U+F131 microphone-slash */
#define ICON_LINK    "\xEF\x87\xA6"     /* U+F1E6 plug             */

#define HIST      60                     /* chart points */
#define LOG_LINES 13
#define NBARS     20                     /* voice bars under the orb */
#define SAVER_MS  (3 * 60 * 1000)

static ui_action_cb_t s_on_action;
static ui_mute_cb_t   s_on_mute;

/* status bar */
static lv_obj_t *lbl_mic, *lbl_link, *lbl_clock;
static lv_obj_t *page_dot[NPAGES];
static lv_obj_t *tv;
static lv_obj_t *tiles[NPAGES];

/* core */
static lv_obj_t *vu_ring, *dash_ring, *sweep[3], *cross[4], *core_ring, *core_dot;
static lv_scale_section_t *vu_sec;
static lv_style_t st_vu_on;
static lv_obj_t *tag, *tag_br[8], *tag_dot, *lbl_state;
static lv_obj_t *bars[NBARS];
static uint8_t   bar_hist[NBARS];
static lv_obj_t *lbl_heard, *lbl_reply;
static bool      s_heard_set;
static uint8_t   s_state = JS_IDLE;
static bool      s_muted, s_mic_present = true, s_link;

/* system */
static lv_obj_t *g_arc[3], *g_val[3];
static lv_obj_t *chart_sys;
static lv_chart_series_t *ser_cpu, *ser_tmp;
static lv_obj_t *lbl_sysinfo;

/* net */
static lv_obj_t *lbl_host, *lbl_ip, *lbl_counts, *lbl_rx, *lbl_tx, *chart_net;
static lv_chart_series_t *ser_rx, *ser_tx;
static uint32_t rx_hist[HIST], tx_hist[HIST];
static int      net_idx;

/* ops */
static lv_obj_t *log_lbl[LOG_LINES];

/* clock */
static bool     s_time_ok;
static uint32_t s_unix;
static int64_t  s_unix_at_us;
static int16_t  s_tz;

/* ------------------------------------------------------------------ helpers */

/* copy at most len bytes of untrusted text, keeping printable ASCII only */
static void sanitize(char *dst, size_t dstsz, const char *src, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < dstsz; i++) {
        char c = src[i];
        if (c == '\0') break;
        dst[o++] = (c >= 0x20 && c <= 0x7E) ? c : '?';
    }
    dst[o] = '\0';
}

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

/* small, letter-spaced caption such as "YOU SAID" */
static lv_obj_t *caption(lv_obj_t *parent, uint32_t color, const char *txt, int x, int y)
{
    lv_obj_t *l = label(parent, F_CAP, color, txt);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* fixed-size text box: wraps, and ends in "..." when the text does not fit */
static lv_obj_t *text_box(lv_obj_t *parent, const lv_font_t *f, uint32_t color, int x, int y, int w, int lines)
{
    lv_obj_t *l = label(parent, f, color, "");
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, lines * lv_font_get_line_height(f));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

/* plain rectangle with no theme styling, not clickable or scrollable */
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* HUD corner brackets: eight short bars hugging the corners of a w x h box.
 * ofs = -border width so they sit on top of the parent's border. Optionally returns the bars. */
static void brackets(lv_obj_t *parent, int w, int h, int len, uint32_t color, lv_obj_t **out, int ofs)
{
    const int r[8][4] = {
        { 0, 0, len, 2 },         { 0, 0, 2, len },              /* top left     */
        { w - len, 0, len, 2 },   { w - 2, 0, 2, len },          /* top right    */
        { 0, h - 2, len, 2 },     { 0, h - len, 2, len },        /* bottom left  */
        { w - len, h - 2, len, 2 }, { w - 2, h - len, 2, len },  /* bottom right */
    };
    for (int i = 0; i < 8; i++) {
        lv_obj_t *b = box(parent, r[i][0] + ofs, r[i][1] + ofs, r[i][2], r[i][3]);
        lv_obj_set_style_bg_color(b, hex(color), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        if (out) out[i] = b;
    }
}

/* holo panel: dark glass with a hairline border and bright corner brackets */
static lv_obj_t *panel_acc(lv_obj_t *parent, int x, int y, int w, int h, uint32_t accent)
{
    lv_obj_t *p = box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(p, hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, hex(C_EDGE), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 0, 0);
    brackets(p, w, h, 8, accent, NULL, -1);
    return p;
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    return panel_acc(parent, x, y, w, h, C_ACC);
}

static lv_obj_t *dot(lv_obj_t *parent, int size, uint32_t color)
{
    lv_obj_t *d = box(parent, 0, 0, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, hex(color), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

static lv_obj_t *ring(lv_obj_t *parent, int size, int width, uint32_t color, bool show_bg)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, hex(C_DIM), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, show_bg ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_pad_all(a, 0, 0);
    return a;
}

static lv_obj_t *chart(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_chart_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_chart_set_type(c, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(c, HIST);
    lv_chart_set_div_line_count(c, 3, 0);
    lv_chart_set_update_mode(c, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_line_color(c, hex(C_DIM), LV_PART_MAIN);
    lv_obj_set_style_line_width(c, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(c, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(c, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

/* "■ CPU" style legend entry */
static void legend(lv_obj_t *parent, int x, int y, uint32_t color, const char *name)
{
    lv_obj_t *sw = box(parent, x, y + 3, 8, 3);
    lv_obj_set_style_bg_color(sw, hex(color), 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    caption(parent, C_MUTED, name, x + 12, y);
}

static uint32_t state_color(void)
{
    if (!s_mic_present || s_muted) return C_RED;
    switch (s_state) {
    case JS_LISTENING: return C_GREEN;
    case JS_THINKING:  return C_AMBER;
    case JS_SPEAKING:  return C_VIOLET;
    case JS_ASLEEP:    return C_MUTED;
    default:           return C_CYAN;
    }
}

static void fmt_rate(char *out, size_t n, uint32_t bps)
{
    if (bps >= 1000000)      snprintf(out, n, "%lu.%lu MB/s", (unsigned long)(bps / 1000000), (unsigned long)((bps / 100000) % 10));
    else if (bps >= 1000)    snprintf(out, n, "%lu KB/s", (unsigned long)(bps / 1000));
    else                     snprintf(out, n, "%lu B/s", (unsigned long)bps);
}

/* ------------------------------------------------------------- animations */

static void glow_cb(void *obj, int32_t v) { lv_obj_set_style_shadow_spread(obj, v, 0); }
static void scale_rot_cb(void *obj, int32_t v) { lv_scale_set_rotation(obj, v); }
static void opa_cb(void *obj, int32_t v) { lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0); }
static void y_cb(void *obj, int32_t v) { lv_obj_set_y(obj, v); }
static void sweep_cb_single(void *obj, int32_t v) { lv_arc_set_rotation(obj, v); }

static void start_anim(lv_obj_t *o, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms, bool reverse)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    if (reverse) lv_anim_set_playback_duration(&a, ms);
    lv_anim_start(&a);
}

static void once_anim(lv_obj_t *o, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms,
                      uint32_t delay, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}
static void x_cb(void *obj, int32_t v) { lv_obj_set_x(obj, v); }
static void w_cb(void *obj, int32_t v) { lv_obj_set_width(obj, v); }
static void fade_cb(void *obj, int32_t v) { lv_obj_set_style_opa(obj, (lv_opa_t)v, 0); }

/* ------------------------------------------------------------- status bar */

static void set_page_dots(int cur)
{
    for (int i = 0; i < NPAGES; i++) {
        bool on = (i == cur);
        lv_obj_set_width(page_dot[i], on ? 14 : 5);
        lv_obj_set_style_bg_color(page_dot[i], hex(on ? C_CYAN : C_EDGE), 0);
    }
}

static void build_status_bar(lv_obj_t *scr)
{
    /* logo: the "A" in a small rounded frame, then the name */
    lv_obj_t *logo = box(scr, 5, 3, 16, 16);
    lv_obj_set_style_border_color(logo, hex(C_CYAN), 0);
    lv_obj_set_style_border_width(logo, 1, 0);
    lv_obj_set_style_radius(logo, 4, 0);
    lv_obj_t *la = label(logo, F_HEAD, C_CYAN, "A");
    lv_obj_center(la);
    lv_obj_t *name = label(scr, F_HEAD, C_TEXT, "ALISHA");
    lv_obj_set_style_text_letter_space(name, 2, 0);
    lv_obj_set_pos(name, 27, 2);

    /* page indicator: one wide dot for the current page */
    lv_obj_t *dots = box(scr, 128, 8, 64, 6);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 5, 0);
    for (int i = 0; i < NPAGES; i++) {
        page_dot[i] = dot(dots, 5, C_EDGE);
        lv_obj_set_style_radius(page_dot[i], 3, 0);
    }
    set_page_dots(0);

    /* right side: mic, link, clock */
    lbl_mic = label(scr, F_ICON, C_RED, ICON_MIC);
    lv_obj_set_pos(lbl_mic, 224, 4);
    lbl_link = label(scr, F_ICON, C_RED, ICON_LINK);
    lv_obj_set_pos(lbl_link, 243, 4);
    lbl_clock = label(scr, F_HEAD, C_TEXT, "--:--");
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_RIGHT, -6, 2);

    lv_obj_t *line = box(scr, 0, BAR_H - 1, W, 1);
    lv_obj_set_style_bg_color(line, hex(C_EDGE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    /* a light that runs back and forth along the bar's bottom edge */
    lv_obj_t *runner = box(scr, -40, BAR_H - 1, 40, 1);
    lv_obj_set_style_bg_color(runner, hex(C_CYAN), 0);
    lv_obj_set_style_bg_opa(runner, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(runner, hex(C_BG), 0);
    lv_obj_set_style_bg_grad_dir(runner, LV_GRAD_DIR_HOR, 0);
    start_anim(runner, x_cb, -40, W, 2600, true);
}

/* ------------------------------------------------------------ boot sequence */

static void boot_status_cb(lv_timer_t *tm)
{
    static const char *const steps[] = {
        "BOOTING NEURAL CORE", "CALIBRATING MIC ARRAY", "LINKING AUDIO PIPELINE",
        "SECURING USB LINK", "ALL SYSTEMS ONLINE",
    };
    static unsigned n;
    lv_obj_t *l = lv_timer_get_user_data(tm);
    lv_label_set_text(l, steps[n < 5 ? n : 4]);
    n++;
}

static void boot_done_cb(lv_anim_t *a) { lv_obj_delete_async(a->var); }

/* ~3 s start-up splash on the top layer, then it fades away to reveal the HUD */
static void boot_sequence(void)
{
    lv_obj_t *o = box(lv_layer_top(), 0, 0, W, H);
    lv_obj_set_style_bg_color(o, hex(C_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    brackets(o, W, H, 16, C_ACC, NULL, 0);

    lv_obj_t *spin = ring(o, 78, 2, C_CYAN, true);
    lv_arc_set_bg_angles(spin, 0, 360);
    lv_arc_set_angles(spin, 0, 90);
    lv_obj_align(spin, LV_ALIGN_TOP_MID, 0, 30);
    start_anim(spin, sweep_cb_single, 0, 360, 1200, false);
    lv_obj_t *core = box(o, 0, 0, 52, 52);
    lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(core, hex(C_CYAN), 0);
    lv_obj_set_style_border_width(core, 2, 0);
    lv_obj_set_style_shadow_color(core, hex(C_CYAN), 0);
    lv_obj_set_style_shadow_width(core, 26, 0);
    lv_obj_set_style_shadow_opa(core, LV_OPA_60, 0);
    lv_obj_align(core, LV_ALIGN_TOP_MID, 0, 43);
    lv_obj_t *a = label(core, F_CORE, C_WHITE, "A");
    lv_obj_center(a);

    lv_obj_t *title = label(o, F_BIG, C_CYAN, "A.L.I.S.H.A");
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 122);
    lv_obj_t *sub1 = label(o, F_CAP, C_MUTED, "ADVANCED LEARNING INTELLIGENT SYSTEM");
    lv_obj_set_style_text_letter_space(sub1, 1, 0);
    lv_obj_align(sub1, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_t *sub2 = label(o, F_CAP, C_MUTED, "FOR HUMAN ASSISTANCE");
    lv_obj_set_style_text_letter_space(sub2, 1, 0);
    lv_obj_align(sub2, LV_ALIGN_TOP_MID, 0, 163);

    lv_obj_t *track = box(o, 50, 186, 220, 2);
    lv_obj_set_style_bg_color(track, hex(C_DIM), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_t *fill = box(o, 50, 186, 0, 2);
    lv_obj_set_style_bg_color(fill, hex(C_CYAN), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    once_anim(fill, w_cb, 0, 220, 2300, 100, NULL);

    lv_obj_t *st = label(o, F_CAP, C_CYAN, "");
    lv_obj_set_style_text_letter_space(st, 1, 0);
    lv_obj_set_width(st, W);
    lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(st, 0, 196);
    lv_timer_t *tm = lv_timer_create(boot_status_cb, 500, st);
    lv_timer_set_repeat_count(tm, 5);
    boot_status_cb(tm);

    once_anim(o, fade_cb, LV_OPA_COVER, LV_OPA_TRANSP, 450, 2900, boot_done_cb);
}

/* ----------------------------------------------------------------- CORE */

/* Text effects for the conversation cards:
 *   heard: "decrypts" - unrevealed letters flicker as random glyphs, then lock in left to right
 *   reply: types out with a block cursor that blinks for a moment when done */
#define FX_MS 30
static char    fx_heard[PROTO_TEXT_MAX + 1], fx_reply[PROTO_TEXT_MAX + 1];
static size_t  fx_heard_len, fx_heard_pos, fx_reply_len, fx_reply_pos;
static int     fx_blink;
static lv_timer_t *fx_heard_tm, *fx_reply_tm;

static void heard_fx_cb(lv_timer_t *tm)
{
    static const char glyphs[] = "ABCDEF0123456789#$%&*<>/\\=+";
    char b[PROTO_TEXT_MAX + 1];
    size_t step = fx_heard_len / 14 + 1;
    fx_heard_pos = fx_heard_pos + step > fx_heard_len ? fx_heard_len : fx_heard_pos + step;
    for (size_t i = 0; i < fx_heard_len; i++) {
        char c = fx_heard[i];
        b[i] = (i < fx_heard_pos || c == ' ') ? c : glyphs[esp_random() % (sizeof glyphs - 1)];
    }
    b[fx_heard_len] = '\0';
    lv_label_set_text(lbl_heard, b);
    if (fx_heard_pos >= fx_heard_len) lv_timer_pause(tm);
}

static void reply_fx_cb(lv_timer_t *tm)
{
    char b[PROTO_TEXT_MAX + 2];
    if (fx_reply_pos < fx_reply_len) {
        fx_reply_pos = fx_reply_pos + 3 > fx_reply_len ? fx_reply_len : fx_reply_pos + 3;
        memcpy(b, fx_reply, fx_reply_pos);
        b[fx_reply_pos] = '_';
        b[fx_reply_pos + 1] = '\0';
        fx_blink = 0;
    } else {
        fx_blink++;                                      /* blink the cursor ~2 s, then hide it */
        memcpy(b, fx_reply, fx_reply_len);
        bool on = fx_blink < 66 && (fx_blink / 8) % 2 == 0;
        b[fx_reply_len] = on ? '_' : '\0';
        b[fx_reply_len + 1] = '\0';
        if (fx_blink >= 66) lv_timer_pause(tm);
    }
    lv_label_set_text(lbl_reply, b);
}

static void set_brackets_color(lv_obj_t *const br[8], uint32_t c)
{
    for (int i = 0; i < 8; i++) lv_obj_set_style_bg_color(br[i], hex(c), 0);
}

static void apply_core_colors(void)
{
    uint32_t c = state_color();
    lv_style_set_line_color(&st_vu_on, hex(c));
    lv_obj_report_style_change(&st_vu_on);
    for (int i = 0; i < 3; i++) lv_obj_set_style_arc_color(sweep[i], hex(c), LV_PART_INDICATOR);
    lv_obj_set_style_line_color(dash_ring, hex(c), LV_PART_ITEMS);
    lv_obj_set_style_line_color(dash_ring, hex(c), LV_PART_INDICATOR);
    for (int i = 0; i < 4; i++) lv_obj_set_style_bg_color(cross[i], hex(c), 0);
    lv_obj_set_style_border_color(core_ring, hex(c), 0);
    lv_obj_set_style_bg_color(core_dot, hex(c), 0);
    lv_obj_set_style_border_color(core_dot, hex(c), 0);
    lv_obj_set_style_shadow_color(core_dot, hex(c), 0);
    lv_obj_set_style_bg_color(tag, hex(c), 0);
    set_brackets_color(tag_br, c);
    lv_obj_set_style_bg_color(tag_dot, hex(c), 0);
    lv_obj_set_style_text_color(lbl_state, hex(c), 0);
    for (int i = 0; i < NBARS; i++) lv_obj_set_style_bg_color(bars[i], hex(c), 0);

    static const char *const names[] = { "STANDBY", "LISTENING", "THINKING", "SPEAKING", "ASLEEP" };
    if (!s_mic_present)      lv_label_set_text(lbl_state, "NO MIC");
    else if (s_muted)        lv_label_set_text(lbl_state, "MIC OFF");
    else                     lv_label_set_text(lbl_state, names[s_state <= JS_MAX ? s_state : 0]);

    if (!s_heard_set) {      /* placeholder hint until the first command is heard */
        lv_label_set_text(lbl_heard, s_muted ? "Microphone is muted. Press BOOT to unmute."
                                             : "Say \"Alisha\" followed by a command.");
    }
}

static void sweep_cb(void *obj, int32_t v)
{
    (void)obj;
    for (int i = 0; i < 3; i++) lv_arc_set_rotation(sweep[i], v);
}

static void build_core(lv_obj_t *t)
{
    const int cx = 68, cy = 70;          /* orb centre */

    /* outer VU ring: 48 ticks that light up with the microphone level */
    vu_ring = lv_scale_create(t);
    lv_obj_set_size(vu_ring, 136, 136);
    lv_obj_set_pos(vu_ring, cx - 68, cy - 68);
    lv_scale_set_mode(vu_ring, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_total_tick_count(vu_ring, 48);
    lv_scale_set_major_tick_every(vu_ring, 6);
    lv_scale_set_label_show(vu_ring, false);
    lv_scale_set_range(vu_ring, 0, 100);
    lv_scale_set_angle_range(vu_ring, 352);
    lv_scale_set_rotation(vu_ring, 270);
    lv_obj_set_style_arc_width(vu_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_line_color(vu_ring, hex(C_DIM), LV_PART_ITEMS);
    lv_obj_set_style_line_width(vu_ring, 2, LV_PART_ITEMS);
    lv_obj_set_style_length(vu_ring, 6, LV_PART_ITEMS);
    lv_obj_set_style_line_color(vu_ring, hex(C_EDGE), LV_PART_INDICATOR);
    lv_obj_set_style_line_width(vu_ring, 2, LV_PART_INDICATOR);
    lv_obj_set_style_length(vu_ring, 10, LV_PART_INDICATOR);
    lv_style_init(&st_vu_on);
    lv_style_set_line_color(&st_vu_on, hex(C_CYAN));
    lv_style_set_line_width(&st_vu_on, 2);
    vu_sec = lv_scale_add_section(vu_ring);
    lv_scale_section_set_style(vu_sec, LV_PART_ITEMS, &st_vu_on);
    lv_scale_section_set_style(vu_sec, LV_PART_INDICATOR, &st_vu_on);
    lv_scale_section_set_range(vu_sec, 200, 200);          /* nothing lit yet */

    /* radar sweep: three stacked arcs share a head, the longer ones fade into a trail */
    static const lv_opa_t trail_opa[3] = { LV_OPA_COVER, LV_OPA_40, LV_OPA_20 };
    static const int trail_start[3] = { 64, 34, 0 };
    for (int i = 0; i < 3; i++) {
        sweep[i] = ring(t, 112, 3, C_CYAN, false);
        lv_arc_set_bg_angles(sweep[i], 0, 360);
        lv_arc_set_angles(sweep[i], trail_start[i], 80);
        lv_obj_set_style_arc_opa(sweep[i], trail_opa[i], LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(sweep[i], false, LV_PART_INDICATOR);
        lv_obj_set_pos(sweep[i], cx - 56, cy - 56);
    }
    start_anim(sweep[0], sweep_cb, 0, 360, 3200, false);

    /* dashed inner ring turning the other way */
    dash_ring = lv_scale_create(t);
    lv_obj_set_size(dash_ring, 92, 92);
    lv_obj_set_pos(dash_ring, cx - 46, cy - 46);
    lv_scale_set_mode(dash_ring, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_total_tick_count(dash_ring, 24);
    lv_scale_set_major_tick_every(dash_ring, 6);
    lv_scale_set_label_show(dash_ring, false);
    lv_scale_set_angle_range(dash_ring, 345);
    lv_obj_set_style_arc_width(dash_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_line_width(dash_ring, 2, LV_PART_ITEMS);
    lv_obj_set_style_length(dash_ring, 3, LV_PART_ITEMS);
    lv_obj_set_style_line_opa(dash_ring, LV_OPA_50, LV_PART_ITEMS);
    lv_obj_set_style_line_width(dash_ring, 2, LV_PART_INDICATOR);
    lv_obj_set_style_length(dash_ring, 6, LV_PART_INDICATOR);
    start_anim(dash_ring, scale_rot_cb, 360, 0, 12000, false);

    /* crosshair marks between the dashed ring and the core */
    for (int i = 0; i < 4; i++) {
        bool vert = (i % 2) == 0;
        int dx = (i == 1) ? 38 : (i == 3) ? -44 : -1;
        int dy = (i == 0) ? -44 : (i == 2) ? 38 : -1;
        cross[i] = box(t, cx + dx, cy + dy, vert ? 2 : 6, vert ? 6 : 2);
        lv_obj_set_style_bg_opa(cross[i], LV_OPA_COVER, 0);
    }

    core_ring = box(t, cx - 35, cy - 35, 70, 70);
    lv_obj_set_style_radius(core_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(core_ring, 1, 0);
    lv_obj_set_style_border_opa(core_ring, LV_OPA_50, 0);

    core_dot = box(t, cx - 28, cy - 28, 56, 56);
    lv_obj_set_style_radius(core_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(core_dot, LV_OPA_30, 0);
    lv_obj_set_style_border_width(core_dot, 2, 0);
    lv_obj_set_style_shadow_width(core_dot, 24, 0);
    lv_obj_set_style_shadow_opa(core_dot, LV_OPA_60, 0);
    start_anim(core_dot, glow_cb, 0, 5, 1400, true);
    lv_obj_t *a = label(core_dot, F_CORE, C_WHITE, "A");   /* A for Alisha */
    lv_obj_center(a);

    /* state tag: bracketed readout with a blinking status light */
    tag = box(t, cx - 62, 148, 124, 24);
    lv_obj_set_style_bg_opa(tag, LV_OPA_10, 0);
    brackets(tag, 124, 24, 6, C_CYAN, tag_br, 0);
    tag_dot = dot(tag, 6, C_CYAN);
    lv_obj_align(tag_dot, LV_ALIGN_LEFT_MID, 12, 0);
    start_anim(tag_dot, opa_cb, LV_OPA_COVER, LV_OPA_20, 600, true);
    lbl_state = label(tag, F_HEAD, C_CYAN, "STANDBY");
    lv_obj_set_style_text_letter_space(lbl_state, 2, 0);
    lv_obj_align(lbl_state, LV_ALIGN_CENTER, 7, 0);

    /* voice bars: a short scrolling history of the mic level, mirrored around a centre line */
    for (int i = 0; i < NBARS; i++) {
        bars[i] = box(t, cx - (NBARS * 6 - 2) / 2 + i * 6, 197, 4, 2);
        lv_obj_set_style_radius(bars[i], 1, 0);
        lv_obj_set_style_bg_opa(bars[i], (lv_opa_t)(LV_OPA_30 + i * 9), 0);   /* newest on the right is brightest */
    }

    /* conversation cards */
    lv_obj_t *you = panel_acc(t, 142, 4, 174, 82, C_GREEN);
    caption(you, C_GREEN, ">", 10, 5);
    caption(you, C_MUTED, "IN // YOU", 22, 5);
    lv_obj_t *ch1 = label(you, F_CAP, C_EDGE2, "CH.01");
    lv_obj_align(ch1, LV_ALIGN_TOP_RIGHT, -9, 5);
    lbl_heard = text_box(you, F_BODY, C_MUTED, 10, 22, 154, 3);

    lv_obj_t *rep = panel_acc(t, 142, 92, 174, 122, C_CYAN);
    caption(rep, C_CYAN, "<", 10, 5);
    caption(rep, C_MUTED, "OUT // ALISHA", 22, 5);
    lv_obj_t *ch2 = label(rep, F_CAP, C_EDGE2, "CH.02");
    lv_obj_align(ch2, LV_ALIGN_TOP_RIGHT, -9, 5);
    lbl_reply = text_box(rep, F_BODY, C_TEXT, 10, 22, 154, 6);
    lv_label_set_text(lbl_reply, "Systems nominal. Standing by.");

    fx_heard_tm = lv_timer_create(heard_fx_cb, FX_MS, NULL);
    lv_timer_pause(fx_heard_tm);
    fx_reply_tm = lv_timer_create(reply_fx_cb, FX_MS, NULL);
    lv_timer_pause(fx_reply_tm);

    /* scan line sweeping down the page */
    lv_obj_t *scan = box(t, 0, 0, W, 1);
    lv_obj_set_style_bg_color(scan, hex(C_CYAN), 0);
    lv_obj_set_style_bg_opa(scan, LV_OPA_20, 0);
    start_anim(scan, y_cb, 0, PAGE_H, 4200, false);

    apply_core_colors();
}

/* ------------------------------------------------------------------ SYS */

static void build_sys(lv_obj_t *t)
{
    static const char *const caps[3] = { "CPU", "TEMP", "MEMORY" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *p = panel(t, 6 + i * 104, 4, 100, 92);
        g_arc[i] = ring(p, 76, 6, C_CYAN, true);
        lv_arc_set_bg_angles(g_arc[i], 135, 45);
        lv_arc_set_range(g_arc[i], 0, 100);
        lv_obj_align(g_arc[i], LV_ALIGN_TOP_MID, 0, 6);
        g_val[i] = label(p, F_BIG, C_TEXT, "--");
        lv_obj_align(g_val[i], LV_ALIGN_TOP_MID, 0, 32);
        lv_obj_t *c = label(p, F_CAP, C_MUTED, caps[i]);
        lv_obj_set_style_text_letter_space(c, 1, 0);
        lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -5);
    }
    lv_obj_t *cp = panel(t, 6, 102, 308, 88);
    caption(cp, C_MUTED, "LAST 60 S", 10, 5);
    legend(cp, 196, 5, C_CYAN, "CPU");
    legend(cp, 246, 5, C_AMBER, "TEMP");
    chart_sys = chart(cp, 10, 22, 288, 58);
    lv_chart_set_range(chart_sys, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    ser_cpu = lv_chart_add_series(chart_sys, hex(C_CYAN), LV_CHART_AXIS_PRIMARY_Y);
    ser_tmp = lv_chart_add_series(chart_sys, hex(C_AMBER), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart_sys, ser_cpu, 0);
    lv_chart_set_all_value(chart_sys, ser_tmp, 0);

    lbl_sysinfo = label(t, F_BODY, C_MUTED, "UP --   FAN --   DISK --");
    lv_obj_set_width(lbl_sysinfo, 308);
    lv_obj_set_style_text_align(lbl_sysinfo, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_sysinfo, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(lbl_sysinfo, 6, 197);
}

/* ------------------------------------------------------------------ NET */

static void build_net(lv_obj_t *t)
{
    lv_obj_t *p = panel(t, 6, 4, 308, 64);
    lbl_host = label(p, F_HEAD, C_CYAN, "--");
    lv_obj_set_pos(lbl_host, 10, 6);
    lv_obj_set_width(lbl_host, 180);
    lv_label_set_long_mode(lbl_host, LV_LABEL_LONG_DOT);
    lbl_ip = label(p, F_BODY, C_TEXT, "--");
    lv_obj_set_pos(lbl_ip, 10, 26);
    lbl_counts = label(p, F_CAP, C_MUTED, "");
    lv_obj_set_style_text_letter_space(lbl_counts, 1, 0);
    lv_obj_set_pos(lbl_counts, 10, 45);

    caption(p, C_MUTED, "DOWN", 200, 8);
    lbl_rx = label(p, F_BODY, C_GREEN, "--");
    lv_obj_align(lbl_rx, LV_ALIGN_TOP_RIGHT, -10, 6);
    caption(p, C_MUTED, "UP", 200, 30);
    lbl_tx = label(p, F_BODY, C_AMBER, "--");
    lv_obj_align(lbl_tx, LV_ALIGN_TOP_RIGHT, -10, 28);

    lv_obj_t *cp = panel(t, 6, 74, 308, 140);
    caption(cp, C_MUTED, "TRAFFIC", 10, 5);
    legend(cp, 196, 5, C_GREEN, "DOWN");
    legend(cp, 252, 5, C_AMBER, "UP");
    chart_net = chart(cp, 10, 22, 288, 108);
    ser_rx = lv_chart_add_series(chart_net, hex(C_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    ser_tx = lv_chart_add_series(chart_net, hex(C_AMBER), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart_net, ser_rx, 0);
    lv_chart_set_all_value(chart_net, ser_tx, 0);
}

/* ------------------------------------------------------------------ OPS */

static void build_ops(lv_obj_t *t)
{
    lv_obj_t *p = panel(t, 6, 4, 308, 210);
    caption(p, C_MUTED, "ACTIVITY FEED", 10, 5);
    for (int i = 0; i < LOG_LINES; i++) {
        log_lbl[i] = label(p, F_BODY, C_GREEN, "");
        lv_obj_set_pos(log_lbl[i], 10, 20 + i * 14);
        lv_obj_set_width(log_lbl[i], 288);
        lv_label_set_long_mode(log_lbl[i], LV_LABEL_LONG_CLIP);
    }
}

/* ----------------------------------------------------------------- KEYS */

/* This board revision has no touchscreen, so the last page is a guide to the buttons. */
static void build_keys(lv_obj_t *t)
{
    static const struct { const char *key; const char *what; uint32_t color; } rows[] = {
        { "UP",           "Previous page",         C_CYAN  },
        { "DOWN",         "Next page",             C_CYAN  },
        { "HOLD UP",      "Alisha: system status", C_GREEN },
        { "HOLD DOWN",    "DEEP DIVE screensaver", C_AMBER },
        { "BOOT",         "Mute / unmute mic",     C_RED   },
        { "ANY BUTTON",   "Wake from saver",       C_MUTED },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        lv_obj_t *r = box(t, 6, 4 + (int)i * 26, 308, 22);       /* plain row, no corner brackets */
        lv_obj_set_style_bg_color(r, hex(C_PANEL), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(r, 3, 0);
        lv_obj_set_style_border_color(r, hex(rows[i].color), 0);
        lv_obj_set_style_radius(r, 3, 0);
        lv_obj_t *k = label(r, F_HEAD, rows[i].color, rows[i].key);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_t *w = label(r, F_BODY, C_TEXT, rows[i].what);
        lv_obj_align(w, LV_ALIGN_LEFT_MID, 120, 0);
    }
    lv_obj_t *n = panel(t, 6, 162, 308, 52);
    lv_obj_t *n0 = label(n, F_HEAD, C_CYAN, "A.L.I.S.H.A.");
    lv_obj_set_style_text_letter_space(n0, 1, 0);
    lv_obj_set_pos(n0, 10, 4);
    lv_obj_t *n1 = label(n, F_BODY, C_TEXT, "Advanced Learning Intelligent");
    lv_obj_set_pos(n1, 10, 21);
    lv_obj_t *n2 = label(n, F_BODY, C_TEXT, "System for Human Assistance");
    lv_obj_set_pos(n2, 10, 34);
    caption(n, C_MUTED, "HOLD = 1 s", 226, 6);
}

/* ------------------------------------------------------------ housekeeping */

static int active_page(void)
{
    lv_obj_t *act = lv_tileview_get_tile_active(tv);
    for (int i = 0; i < NPAGES; i++) if (tiles[i] == act) return i;
    return 0;
}

static void tick_1s(lv_timer_t *tm)
{
    (void)tm;
    static bool colon;
    char buf[40];
    if (s_time_ok) {
        int64_t el = (esp_timer_get_time() - s_unix_at_us) / 1000000;
        time_t now = (time_t)s_unix + (time_t)el + (time_t)s_tz * 60;
        struct tm tmv;
        gmtime_r(&now, &tmv);
        colon = !colon;                                   /* blinking colon */
        snprintf(buf, sizeof buf, "%02d%c%02d", tmv.tm_hour, colon ? ':' : ' ', tmv.tm_min);
        lv_label_set_text(lbl_clock, buf);
    }
    bool idle = (s_state == JS_IDLE || s_state == JS_ASLEEP);
    if (idle && !saver_visible() && lv_display_get_inactive_time(NULL) > SAVER_MS) {
        saver_show();
    }
}

static void page_changed_cb(lv_event_t *e)
{
    (void)e;
    set_page_dots(active_page());
}

void ui_init(ui_action_cb_t on_action, ui_mute_cb_t on_mute_toggle)
{
    s_on_action = on_action;
    s_on_mute = on_mute_toggle;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_status_bar(scr);

    tv = lv_tileview_create(scr);
    lv_obj_set_pos(tv, 0, BAR_H);
    lv_obj_set_size(tv, W, PAGE_H);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < NPAGES; i++) {
        lv_dir_t dir = (i == 0 ? LV_DIR_RIGHT : i == NPAGES - 1 ? LV_DIR_LEFT : LV_DIR_HOR);
        tiles[i] = lv_tileview_add_tile(tv, (uint8_t)i, 0, dir);
        lv_obj_set_scrollbar_mode(tiles[i], LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(tiles[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_add_event_cb(tv, page_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    build_core(tiles[0]);
    build_sys(tiles[1]);
    build_net(tiles[2]);
    build_ops(tiles[3]);
    build_keys(tiles[4]);
    saver_create();
    boot_sequence();

    lv_timer_create(tick_1s, 1000, NULL);
}

/* ----------------------------------------------------------- public API */

void ui_set_stats(const hud_stats_t *s)
{
    char b[64], ip[17], host[17], rx[16], tx[16];
    sanitize(ip, sizeof ip, s->ip, sizeof s->ip);
    sanitize(host, sizeof host, s->host, sizeof s->host);
    int temp = s->temp_c_x10 / 10;
    int vals[3] = { s->cpu_pct, temp < 0 ? 0 : (temp > 100 ? 100 : temp), s->mem_pct };

    if (!lvgl_port_lock(100)) return;
    for (int i = 0; i < 3; i++) {
        int v = vals[i] > 100 ? 100 : vals[i];
        lv_arc_set_value(g_arc[i], v);
        uint32_t c = v >= 85 ? C_RED : v >= 65 ? C_AMBER : C_CYAN;
        lv_obj_set_style_arc_color(g_arc[i], hex(c), LV_PART_INDICATOR);
        snprintf(b, sizeof b, i == 1 ? "%dC" : "%d%%", i == 1 ? temp : v);
        lv_label_set_text(g_val[i], b);
    }
    lv_chart_set_next_value(chart_sys, ser_cpu, vals[0]);
    lv_chart_set_next_value(chart_sys, ser_tmp, vals[1]);

    uint32_t up = s->uptime_s;
    snprintf(b, sizeof b, "UP %lud %luh   FAN %u rpm   DISK %u%%",
             (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
             (unsigned)s->fan_rpm, (unsigned)s->disk_pct);
    lv_label_set_text(lbl_sysinfo, b);

    lv_label_set_text(lbl_host, host[0] ? host : "--");
    lv_label_set_text(lbl_ip, ip[0] ? ip : "no address");
    snprintf(b, sizeof b, "LAN %u  PORTS %u  SSH %u", (unsigned)s->lan_hosts,
             (unsigned)s->listen_ports, (unsigned)s->ssh_sessions);
    lv_label_set_text(lbl_counts, b);
    lv_obj_set_style_text_color(lbl_counts, hex(s->ssh_sessions ? C_AMBER : C_MUTED), 0);
    fmt_rate(rx, sizeof rx, s->rx_bps);
    fmt_rate(tx, sizeof tx, s->tx_bps);
    lv_label_set_text(lbl_rx, rx);
    lv_label_set_text(lbl_tx, tx);

    rx_hist[net_idx] = s->rx_bps;
    tx_hist[net_idx] = s->tx_bps;
    net_idx = (net_idx + 1) % HIST;
    uint32_t mx = 1024;
    for (int i = 0; i < HIST; i++) {
        if (rx_hist[i] > mx) mx = rx_hist[i];
        if (tx_hist[i] > mx) mx = tx_hist[i];
    }
    /* chart values are scaled to 0..1000 of the window max so int32 never overflows */
    lv_chart_set_range(chart_net, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    for (int i = 0; i < HIST; i++) {
        int k = (net_idx + i) % HIST;
        lv_chart_set_value_by_id(chart_net, ser_rx, (uint32_t)i, (int32_t)((uint64_t)rx_hist[k] * 1000 / mx));
        lv_chart_set_value_by_id(chart_net, ser_tx, (uint32_t)i, (int32_t)((uint64_t)tx_hist[k] * 1000 / mx));
    }
    lv_chart_refresh(chart_net);
    saver_set_identity(host, ip, s->uptime_s, s->cpu_pct, s->temp_c_x10);
    lvgl_port_unlock();
}

void ui_set_state(uint8_t js)
{
    if (js > JS_MAX) return;
    if (!lvgl_port_lock(100)) return;
    if (js != s_state) once_anim(tag, opa_cb, LV_OPA_70, LV_OPA_10, 450, 0, NULL);   /* flash on change */
    s_state = js;
    apply_core_colors();
    if (js == JS_LISTENING || js == JS_SPEAKING) {
        saver_hide();
        lv_tileview_set_tile_by_index(tv, 0, 0, LV_ANIM_ON);
        set_page_dots(0);
        lv_display_trigger_activity(NULL);
    }
    lvgl_port_unlock();
}

void ui_log(uint8_t level, const char *text, size_t len)
{
    static const uint32_t colors[] = { C_MUTED, C_GREEN, C_AMBER, C_RED };
    static const char *const tags[] = { " ", "+", "!", "x" };
    char clean[PROTO_LOG_MAX + 1];
    char line[PROTO_LOG_MAX + 16];
    if (level > LOG_LVL_MAX) level = LOG_INFO;
    sanitize(clean, sizeof clean, text, len);
    snprintf(line, sizeof line, "%s %s", tags[level], clean);

    if (!lvgl_port_lock(100)) return;
    for (int i = 0; i < LOG_LINES - 1; i++) {   /* scroll up */
        lv_label_set_text(log_lbl[i], lv_label_get_text(log_lbl[i + 1]));
        lv_obj_set_style_text_color(log_lbl[i], lv_obj_get_style_text_color(log_lbl[i + 1], 0), 0);
    }
    lv_label_set_text(log_lbl[LOG_LINES - 1], line);
    lv_obj_set_style_text_color(log_lbl[LOG_LINES - 1], hex(colors[level]), 0);
    if (level == LOG_ALERT) {
        saver_hide();
        lv_tileview_set_tile_by_index(tv, 3, 0, LV_ANIM_ON);
        set_page_dots(3);
        lv_display_trigger_activity(NULL);
    }
    lvgl_port_unlock();
}

void ui_set_heard(const char *text, size_t len)
{
    char b[PROTO_TEXT_MAX + 1];
    sanitize(b, sizeof b, text, len);
    if (!lvgl_port_lock(100)) return;
    s_heard_set = b[0] != '\0';
    lv_obj_set_style_text_color(lbl_heard, hex(s_heard_set ? C_TEXT : C_MUTED), 0);
    if (s_heard_set) {
        memcpy(fx_heard, b, sizeof fx_heard);
        fx_heard_len = strlen(fx_heard);
        fx_heard_pos = 0;
        lv_timer_resume(fx_heard_tm);
    } else {
        lv_timer_pause(fx_heard_tm);
        apply_core_colors();          /* puts the hint back */
    }
    lvgl_port_unlock();
}

void ui_set_reply(const char *text, size_t len)
{
    char b[PROTO_TEXT_MAX + 1];
    sanitize(b, sizeof b, text, len);
    if (!lvgl_port_lock(100)) return;
    if (!b[0]) memcpy(b, "...", 4);
    memcpy(fx_reply, b, sizeof fx_reply);
    fx_reply_len = strlen(fx_reply);
    fx_reply_pos = 0;
    fx_blink = 0;
    lv_timer_resume(fx_reply_tm);
    lvgl_port_unlock();
}

void ui_set_level(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (!lvgl_port_lock(20)) return;
    if (pct >= 2) lv_scale_section_set_range(vu_sec, 0, pct);
    else          lv_scale_section_set_range(vu_sec, 200, 200);    /* all ticks dark */
    lv_obj_invalidate(vu_ring);
    memmove(bar_hist, bar_hist + 1, NBARS - 1);       /* scroll the voice bars left */
    bar_hist[NBARS - 1] = pct;
    for (int i = 0; i < NBARS; i++) {
        int h = 2 + bar_hist[i] * 30 / 100;
        lv_obj_set_height(bars[i], h);
        lv_obj_set_y(bars[i], 197 - h / 2);
    }
    lvgl_port_unlock();
}

void ui_set_link(bool up)
{
    if (!lvgl_port_lock(100)) return;
    if (up != s_link) {
        s_link = up;
        lv_obj_set_style_text_color(lbl_link, hex(up ? C_GREEN : C_RED), 0);
    }
    lvgl_port_unlock();
}

void ui_set_mic(bool muted, bool present)
{
    if (!lvgl_port_lock(100)) return;
    s_muted = muted;
    s_mic_present = present;
    lv_obj_set_style_text_color(lbl_mic, hex((muted || !present) ? C_RED : C_GREEN), 0);
    lv_label_set_text(lbl_mic, (muted || !present) ? ICON_MIC_OFF : ICON_MIC);
    apply_core_colors();
    lvgl_port_unlock();
}

void ui_set_time(uint32_t unix_time, int16_t utc_offset_min)
{
    if (!lvgl_port_lock(100)) return;
    s_unix = unix_time;
    s_unix_at_us = esp_timer_get_time();
    s_tz = utc_offset_min;
    s_time_ok = true;
    lvgl_port_unlock();
}

void ui_saver(bool on)
{
    if (!lvgl_port_lock(100)) return;
    if (on) saver_show();
    else    saver_hide();
    lv_display_trigger_activity(NULL);
    lvgl_port_unlock();
}

bool ui_page_step(int dir)
{
    bool moved = false;
    if (!lvgl_port_lock(100)) return false;
    lv_display_trigger_activity(NULL);
    if (saver_visible()) {
        saver_hide();                   /* first press just wakes the HUD */
    } else {
        int cur = (active_page() + dir + NPAGES) % NPAGES;
        lv_tileview_set_tile_by_index(tv, (uint32_t)cur, 0, LV_ANIM_ON);
        set_page_dots(cur);
        moved = true;
    }
    lvgl_port_unlock();
    return moved;
}

void ui_set_hosts(const uint8_t (*ips)[4], int n)
{
    if (!lvgl_port_lock(100)) return;
    saver_set_hosts(ips, n);
    lvgl_port_unlock();
}
