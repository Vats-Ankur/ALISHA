/* "DEEP DIVE" — a hacker-console screensaver that runs on REAL data from your setup.
 *
 *  1. RADAR    network sweep; the blips are the actual hosts in the Pi's ARP table
 *  2. GLOBE    spinning wireframe globe with traceroute arcs firing between nodes
 *  3. DECRYPT  hex dump + payload decrypting letter by letter into your node's identity
 *  4. SIGNAL   live oscilloscope + voiceprint from the board's own microphone
 *
 * Between scenes: a glitch transition. Always on: CRT scanlines, corner brackets,
 * a status ticker and a terminal that types along the bottom.
 * All original; drawn only with LVGL primitives.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "saver.h"

#define W 320
#define H 240
#define TICK_MS 40
#define SCENE_TICKS 225          /* 9 s per scene */
#define GLITCH_TICKS 9
#define N_SCENES 4

#define C_BG     0x010604
#define C_GREEN  0x39FF14
#define C_DGREEN 0x0E4A22
#define C_CYAN   0x00E5FF
#define C_MAG    0xFF2AD4
#define C_AMBER  0xFFB000
#define C_RED    0xFF2A2A
#define C_WHITE  0xD8FFE8
#define F8  (&lv_font_unscii_8)
#define F16 (&lv_font_unscii_16)

/* ------------------------------------------------------------ live data */
#define MAX_HOSTS 32
#define WAVE_N 64
static uint8_t  s_hosts[MAX_HOSTS][4];
static int      s_nhosts;
static char     s_host[17] = "kali";
static char     s_ip[17] = "0.0.0.0";
static uint32_t s_uptime;
static uint8_t  s_cpu;
static int16_t  s_temp_x10;
static volatile int16_t s_wave[WAVE_N];
static volatile uint8_t s_level;
static volatile bool    s_muted;

/* ---------------------------------------------------------------- objects */
static lv_obj_t   *layer, *stage, *scene[N_SCENES], *glitch_bar[7];
static lv_obj_t   *lbl_top, *lbl_term, *lbl_scene;
static lv_timer_t *timer;
static int         t, cur, glitch;

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }
static int rnd(int n) { return (int)(esp_random() % (uint32_t)n); }

static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, uint32_t c, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, hex(c), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *txt(lv_obj_t *p, const lv_font_t *f, uint32_t c, int x, int y, const char *s)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, hex(c), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, s);
    return l;
}

static lv_obj_t *ring(lv_obj_t *p, int cx, int cy, int r, uint32_t c, lv_opa_t opa)
{
    lv_obj_t *o = box(p, cx - r, cy - r, 2 * r, 2 * r, c, LV_OPA_TRANSP);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(o, hex(c), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_opa(o, opa, 0);
    return o;
}

static lv_obj_t *line(lv_obj_t *p, uint32_t c, int w, lv_opa_t opa)
{
    lv_obj_t *l = lv_line_create(p);
    lv_obj_set_style_line_color(l, hex(c), 0);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_opa(l, opa, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* scrambles text towards `target`: chars lock in left-to-right as `progress` goes 0..len */
static void decrypt_text(char *out, const char *target, int progress)
{
    static const char glyph[] = "!#$%&*+<>?@ABCDEFGHJKLMNPQRSTUVWXYZ0123456789/\\|=";
    int n = (int)strlen(target);
    for (int i = 0; i < n; i++) {
        out[i] = (i < progress || target[i] == ' ') ? target[i] : glyph[rnd(sizeof glyph - 1)];
    }
    out[n] = '\0';
}

/* ================================================================ RADAR */
#define RCX 104
#define RCY 112
#define RR  86
static lv_obj_t *r_wedge[3], *r_sweep, *r_blip[MAX_HOSTS], *r_tag[MAX_HOSTS], *r_list, *r_count;
static lv_point_precise_t r_sweep_pts[2];
static uint8_t  r_glow[MAX_HOSTS];

static void host_polar(int i, int *x, int *y, int *ang)
{
    /* stable pseudo-random position per address so a host keeps its spot */
    uint32_t h = (uint32_t)s_hosts[i][3] * 2654435761u ^ (uint32_t)s_hosts[i][2] * 40503u;
    int a = (int)(h % 360);
    int r = 22 + (int)((h >> 9) % (RR - 30));
    *ang = a;
    *x = RCX + (int)(r * cosf(a * (float)M_PI / 180.f));
    *y = RCY + (int)(r * sinf(a * (float)M_PI / 180.f));
}

static void radar_build(lv_obj_t *s)
{
    for (int i = 0; i < 3; i++) {                     /* phosphor trail behind the beam */
        r_wedge[i] = lv_arc_create(s);
        lv_obj_set_size(r_wedge[i], 2 * RR, 2 * RR);
        lv_obj_set_pos(r_wedge[i], RCX - RR, RCY - RR);
        lv_obj_remove_style(r_wedge[i], NULL, LV_PART_KNOB);
        lv_obj_remove_flag(r_wedge[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_opa(r_wedge[i], LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(r_wedge[i], RR, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(r_wedge[i], false, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(r_wedge[i], hex(C_GREEN), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(r_wedge[i], (lv_opa_t)(70 - i * 22), LV_PART_INDICATOR);
        lv_arc_set_bg_angles(r_wedge[i], 0, 360);
        lv_arc_set_angles(r_wedge[i], 0, 14 + i * 18);
    }
    for (int k = 1; k <= 4; k++) ring(s, RCX, RCY, RR * k / 4, C_GREEN, k == 4 ? LV_OPA_80 : LV_OPA_40);
    box(s, RCX - RR, RCY, 2 * RR, 1, C_GREEN, LV_OPA_30);
    box(s, RCX, RCY - RR, 1, 2 * RR, C_GREEN, LV_OPA_30);
    for (int a = 0; a < 360; a += 30) {               /* bearing ticks */
        int x = RCX + (int)((RR + 3) * cosf(a * (float)M_PI / 180.f));
        int y = RCY + (int)((RR + 3) * sinf(a * (float)M_PI / 180.f));
        box(s, x - 1, y - 1, 2, 2, C_GREEN, LV_OPA_70);
    }
    r_sweep = line(s, C_WHITE, 2, LV_OPA_90);
    for (int i = 0; i < MAX_HOSTS; i++) {
        r_blip[i] = box(s, 0, 0, 5, 5, C_GREEN, LV_OPA_COVER);
        lv_obj_set_style_radius(r_blip[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_shadow_color(r_blip[i], hex(C_GREEN), 0);
        lv_obj_set_style_shadow_width(r_blip[i], 8, 0);
        r_tag[i] = txt(s, F8, C_GREEN, 0, 0, "");
        show(r_blip[i], false);
        show(r_tag[i], false);
    }
    txt(s, F8, C_CYAN, 200, 22, "NET RECON");
    r_count = txt(s, F16, C_WHITE, 200, 34, "00 HOSTS");
    r_list = txt(s, F8, C_GREEN, 200, 56, "");
    lv_obj_set_style_text_line_space(r_list, 3, 0);
}

static void radar_step(void)
{
    int ang = (t * 6) % 360;                          /* one revolution per 2.4 s */
    for (int i = 0; i < 3; i++) lv_arc_set_rotation(r_wedge[i], (ang - 14 - i * 18 + 360) % 360);
    r_sweep_pts[0] = (lv_point_precise_t){ RCX, RCY };
    r_sweep_pts[1] = (lv_point_precise_t){ RCX + (int)(RR * cosf(ang * (float)M_PI / 180.f)),
                                           RCY + (int)(RR * sinf(ang * (float)M_PI / 180.f)) };
    lv_line_set_points(r_sweep, r_sweep_pts, 2);

    char list[200] = "";
    int n = s_nhosts;
    for (int i = 0; i < MAX_HOSTS; i++) {
        if (i >= n) { show(r_blip[i], false); show(r_tag[i], false); continue; }
        int x, y, a;
        host_polar(i, &x, &y, &a);
        int d = (ang - a + 360) % 360;
        if (d < 8) r_glow[i] = 255;                   /* beam just passed: light up */
        else if (r_glow[i] > 40) r_glow[i] -= 6;
        bool self = memcmp(s_hosts[i], s_hosts[0], 3) == 0 && s_hosts[i][3] == 1;   /* gateway */
        uint32_t c = self ? C_AMBER : C_GREEN;
        lv_obj_set_pos(r_blip[i], x - 2, y - 2);
        lv_obj_set_style_bg_color(r_blip[i], hex(c), 0);
        lv_obj_set_style_shadow_color(r_blip[i], hex(c), 0);
        lv_obj_set_style_opa(r_blip[i], r_glow[i], 0);
        char tag[6];
        snprintf(tag, sizeof tag, ".%u", s_hosts[i][3]);
        lv_label_set_text(r_tag[i], tag);
        lv_obj_set_pos(r_tag[i], x + 4, y - 4);
        lv_obj_set_style_opa(r_tag[i], r_glow[i], 0);
        show(r_blip[i], true);
        show(r_tag[i], true);
        if (i < 14) {
            char row[24];
            snprintf(row, sizeof row, "%s%3u.%-3u %s\n", r_glow[i] > 200 ? ">" : " ",
                     s_hosts[i][2], s_hosts[i][3], self ? "GW" : (r_glow[i] > 200 ? "PING" : "up"));
            strncat(list, row, sizeof list - strlen(list) - 1);
        }
    }
    if (n == 0) snprintf(list, sizeof list, " awaiting\n ARP data\n from Pi...");
    lv_label_set_text(r_list, list);
    char c[24];
    snprintf(c, sizeof c, "%02d HOSTS", n);
    lv_label_set_text(r_count, c);
}

/* ================================================================ GLOBE */
#define GCX 112
#define GCY 114
#define GR  80
#define LAT_N 5
#define MER_N 8
#define RING_PTS 25
#define N_CITY 12
#define ARC_PTS 20
static lv_obj_t *g_lat[LAT_N], *g_mer[MER_N], *g_city[N_CITY], *g_arc[2], *g_ping[2], *g_info;
static lv_point_precise_t g_lat_pts[LAT_N][RING_PTS], g_mer_pts[MER_N][RING_PTS], g_arc_pts[2][ARC_PTS];
static const float CITY[N_CITY][2] = {       /* lat, lon (deg) - just scattered nodes */
    { 51, 0 }, { 40, -74 }, { 35, 139 }, { -33, 151 }, { 1, 103 }, { 55, 37 },
    { -23, -46 }, { 37, -122 }, { 19, 72 }, { 30, 31 }, { 60, 25 }, { -34, 18 },
};
static int g_from[2], g_to[2], g_t0[2];

static void sphere(float lat, float lon, float rot, float alt, float *x, float *y, float *z)
{
    const float tilt = 0.38f;                 /* ~22 degrees toward the viewer */
    float la = lat * (float)M_PI / 180.f, lo = (lon + rot) * (float)M_PI / 180.f;
    float px = cosf(la) * sinf(lo), py = sinf(la), pz = cosf(la) * cosf(lo);
    float y2 = py * cosf(tilt) - pz * sinf(tilt);
    float z2 = py * sinf(tilt) + pz * cosf(tilt);
    *x = GCX + GR * alt * px;
    *y = GCY - GR * alt * y2;
    *z = z2;
}

static void globe_build(lv_obj_t *s)
{
    ring(s, GCX, GCY, GR + 6, C_CYAN, LV_OPA_20);
    for (int i = 0; i < LAT_N; i++) g_lat[i] = line(s, C_DGREEN, 1, LV_OPA_COVER);
    for (int i = 0; i < MER_N; i++) g_mer[i] = line(s, C_DGREEN, 1, LV_OPA_COVER);
    for (int i = 0; i < 2; i++) {
        g_arc[i] = line(s, i ? C_MAG : C_AMBER, 2, LV_OPA_COVER);
        g_ping[i] = ring(s, 0, 0, 4, i ? C_MAG : C_AMBER, LV_OPA_COVER);
        g_from[i] = rnd(N_CITY);
        g_to[i] = (g_from[i] + 1 + rnd(N_CITY - 1)) % N_CITY;
        g_t0[i] = -i * 25;
    }
    for (int i = 0; i < N_CITY; i++) {
        g_city[i] = box(s, 0, 0, 4, 4, C_CYAN, LV_OPA_COVER);
        lv_obj_set_style_radius(g_city[i], 2, 0);
    }
    txt(s, F8, C_CYAN, 210, 22, "TRACE ROUTE");
    g_info = txt(s, F8, C_GREEN, 210, 36, "");
    lv_obj_set_style_text_line_space(g_info, 3, 0);
}

static void globe_step(void)
{
    float rot = t * 1.6f, x, y, z;
    static const float lats[LAT_N] = { -60, -30, 0, 30, 60 };
    for (int i = 0; i < LAT_N; i++) {
        for (int k = 0; k < RING_PTS; k++) {
            sphere(lats[i], k * 15.f, rot, 1, &x, &y, &z);
            g_lat_pts[i][k] = (lv_point_precise_t){ (int)x, (int)y };
        }
        lv_line_set_points(g_lat[i], g_lat_pts[i], RING_PTS);
    }
    for (int i = 0; i < MER_N; i++) {
        for (int k = 0; k < RING_PTS; k++) {
            sphere(-90 + k * 7.5f, i * 22.5f, rot, 1, &x, &y, &z);
            g_mer_pts[i][k] = (lv_point_precise_t){ (int)x, (int)y };
        }
        lv_line_set_points(g_mer[i], g_mer_pts[i], RING_PTS);
    }
    for (int i = 0; i < N_CITY; i++) {
        sphere(CITY[i][0], CITY[i][1], rot, 1, &x, &y, &z);
        lv_obj_set_pos(g_city[i], (int)x - 2, (int)y - 2);
        lv_obj_set_style_opa(g_city[i], z > 0 ? LV_OPA_COVER : LV_OPA_20, 0);
    }

    char info[220] = "";
    for (int a = 0; a < 2; a++) {
        int p = t - g_t0[a];
        if (p > 70) {                                    /* next hop */
            g_from[a] = g_to[a];
            g_to[a] = (g_to[a] + 1 + rnd(N_CITY - 1)) % N_CITY;
            g_t0[a] = t;
            p = 0;
        }
        if (p < 0) { show(g_arc[a], false); show(g_ping[a], false); continue; }
        /* great-circle arc lifted off the surface, drawn progressively */
        float la1 = CITY[g_from[a]][0] * (float)M_PI / 180.f, lo1 = CITY[g_from[a]][1] * (float)M_PI / 180.f;
        float la2 = CITY[g_to[a]][0] * (float)M_PI / 180.f, lo2 = CITY[g_to[a]][1] * (float)M_PI / 180.f;
        float ax = cosf(la1) * cosf(lo1), ay = cosf(la1) * sinf(lo1), az = sinf(la1);
        float bx = cosf(la2) * cosf(lo2), by = cosf(la2) * sinf(lo2), bz = sinf(la2);
        float om = acosf(fmaxf(-1.f, fminf(1.f, ax * bx + ay * by + az * bz)));
        int n = p < 25 ? 2 + p * (ARC_PTS - 2) / 25 : ARC_PTS;
        for (int k = 0; k < n; k++) {
            float u = (float)k / (ARC_PTS - 1);
            float s1 = om > 1e-3f ? sinf((1 - u) * om) / sinf(om) : 1 - u;
            float s2 = om > 1e-3f ? sinf(u * om) / sinf(om) : u;
            float cx = s1 * ax + s2 * bx, cy = s1 * ay + s2 * by, cz = s1 * az + s2 * bz;
            float lat = asinf(fmaxf(-1.f, fminf(1.f, cz))) * 180.f / (float)M_PI;
            float lon = atan2f(cy, cx) * 180.f / (float)M_PI;
            sphere(lat, lon, rot, 1.f + 0.28f * sinf(u * (float)M_PI), &x, &y, &z);
            g_arc_pts[a][k] = (lv_point_precise_t){ (int)x, (int)y };
        }
        lv_line_set_points(g_arc[a], g_arc_pts[a], (uint32_t)n);
        lv_obj_set_style_line_opa(g_arc[a], p > 55 ? (lv_opa_t)(255 - (p - 55) * 16) : LV_OPA_COVER, 0);
        show(g_arc[a], true);
        if (p >= 25 && p < 55) {                          /* impact ping at the target */
            int r = 3 + (p - 25) / 2;
            lv_obj_set_size(g_ping[a], 2 * r, 2 * r);
            lv_point_precise_t e = g_arc_pts[a][ARC_PTS - 1];
            lv_obj_set_pos(g_ping[a], (int)e.x - r, (int)e.y - r);
            lv_obj_set_style_border_opa(g_ping[a], (lv_opa_t)(255 - (p - 25) * 8), 0);
            show(g_ping[a], true);
        } else {
            show(g_ping[a], false);
        }
        char row[100];
        snprintf(row, sizeof row, "%s NODE-%02d\n -> NODE-%02d\n hop %d  %3dms\n %s\n\n",
                 a ? "[M]" : "[A]", g_from[a], g_to[a], 3 + (t / 70 + a) % 9, 18 + (p * 7 + a * 31) % 160,
                 p < 25 ? "routing..." : "LINK UP");
        strncat(info, row, sizeof info - strlen(info) - 1);
    }
    lv_label_set_text(g_info, info);
}

/* ============================================================== DECRYPT */
#define HEX_LINES 18
static lv_obj_t *d_hex[HEX_LINES], *d_title, *d_line, *d_bar, *d_pct, *d_frame;
static char d_target[48];

static void hex_row(char *out, size_t n)
{
    static uint32_t addr = 0x7f3a0000;
    char *p = out;
    p += snprintf(p, n, "%08lx ", (unsigned long)addr);
    char ascii[9];
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)rnd(256);
        p += snprintf(p, n - (size_t)(p - out), "%02x ", b);
        ascii[i] = (b >= 0x21 && b < 0x7f) ? (char)b : '.';
    }
    ascii[8] = '\0';
    snprintf(p, n - (size_t)(p - out), "%s", ascii);
    addr += 8;
}

static void decrypt_build(lv_obj_t *s)
{
    for (int i = 0; i < HEX_LINES; i++) {
        char r[64];
        hex_row(r, sizeof r);
        d_hex[i] = txt(s, F8, C_DGREEN, 4, 20 + i * 11, r);
    }
    d_frame = box(s, 22, 74, 276, 92, C_BG, LV_OPA_90);
    lv_obj_set_style_border_color(d_frame, hex(C_GREEN), 0);
    lv_obj_set_style_border_width(d_frame, 1, 0);
    lv_obj_set_style_shadow_color(d_frame, hex(C_GREEN), 0);
    lv_obj_set_style_shadow_width(d_frame, 18, 0);
    lv_obj_set_style_shadow_opa(d_frame, LV_OPA_40, 0);
    d_title = txt(d_frame, F8, C_CYAN, 10, 8, "DECRYPTING PAYLOAD  AES-256/GCM");
    d_line = txt(d_frame, F16, C_WHITE, 10, 28, "");
    box(d_frame, 10, 58, 220, 8, C_DGREEN, LV_OPA_COVER);
    d_bar = box(d_frame, 10, 58, 1, 8, C_GREEN, LV_OPA_COVER);
    d_pct = txt(d_frame, F8, C_GREEN, 236, 58, "0%");
    txt(d_frame, F8, C_DGREEN, 10, 74, "key schedule: 14 rounds  sbox: ok");
}

static void decrypt_step(void)
{
    if (t % 2 == 0) {                                     /* scroll the hex dump */
        for (int i = 0; i < HEX_LINES - 1; i++) lv_label_set_text(d_hex[i], lv_label_get_text(d_hex[i + 1]));
        char r[64];
        hex_row(r, sizeof r);
        lv_label_set_text(d_hex[HEX_LINES - 1], r);
    }
    /* three reveals per scene: identity, address, uptime */
    int phase = (t % SCENE_TICKS) / 75, p = (t % SCENE_TICKS) % 75;
    if (p == 0) {
        if (phase == 0) snprintf(d_target, sizeof d_target, "ACCESS GRANTED");
        else if (phase == 1) snprintf(d_target, sizeof d_target, "NODE %.12s", s_host);
        else snprintf(d_target, sizeof d_target, "%s", s_ip);
        for (char *c = d_target; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    }
    int len = (int)strlen(d_target);
    int prog = p < 10 ? 0 : (p - 10) * len / 40;
    char out[48];
    decrypt_text(out, d_target, prog > len ? len : prog);
    lv_label_set_text(d_line, out);
    bool done = prog >= len;
    lv_obj_set_style_text_color(d_line, hex(done ? C_GREEN : C_WHITE), 0);
    int pct = done ? 100 : (p * 100 / 50 > 99 ? 99 : p * 100 / 50);
    lv_obj_set_width(d_bar, 1 + 219 * pct / 100);
    char pc[24];
    if (done && phase == 2) {
        snprintf(pc, sizeof pc, "UP %lud%02luh", (unsigned long)(s_uptime / 86400), (unsigned long)(s_uptime / 3600 % 24));
    } else {
        snprintf(pc, sizeof pc, "%d%%", pct);
    }
    lv_label_set_text(d_pct, pc);
    lv_obj_set_style_border_color(d_frame, hex(done && (t / 3) % 2 ? C_WHITE : C_GREEN), 0);
}

/* =============================================================== SIGNAL */
#define BARS 40
static lv_obj_t *s_scope, *s_bar[BARS], *s_db, *s_state;
static lv_point_precise_t s_scope_pts[WAVE_N];
static uint8_t s_hist[BARS];

static void signal_build(lv_obj_t *s)
{
    txt(s, F8, C_CYAN, 6, 22, "SIGNAL INTERCEPT  //  MIC CH0  16kHz");
    lv_obj_t *frame = box(s, 6, 36, 308, 84, C_BG, LV_OPA_TRANSP);
    lv_obj_set_style_border_color(frame, hex(C_DGREEN), 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    for (int i = 1; i < 4; i++) box(s, 6, 36 + i * 21, 308, 1, C_DGREEN, LV_OPA_60);
    for (int i = 1; i < 8; i++) box(s, 6 + i * 38, 36, 1, 84, C_DGREEN, LV_OPA_60);
    s_scope = line(s, C_GREEN, 2, LV_OPA_COVER);
    lv_obj_set_style_shadow_color(s_scope, hex(C_GREEN), 0);
    txt(s, F8, C_CYAN, 6, 126, "VOICEPRINT");
    for (int i = 0; i < BARS; i++) {
        s_bar[i] = box(s, 8 + i * 7 + 1, 200, 5, 2, C_GREEN, LV_OPA_COVER);
    }
    s_db = txt(s, F16, C_WHITE, 200, 124, "-- dB");
    s_state = txt(s, F8, C_GREEN, 110, 128, "");
}

static void signal_step(void)
{
    /* oscilloscope: latest 64-sample snapshot from the audio task */
    int peak = 1;
    for (int i = 0; i < WAVE_N; i++) {
        int v = s_wave[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    int gain = peak < 600 ? 600 : peak;                    /* auto-range, quiet room stays flat-ish */
    for (int i = 0; i < WAVE_N; i++) {
        int v = s_muted ? 0 : s_wave[i];
        s_scope_pts[i] = (lv_point_precise_t){ 8 + i * 304 / (WAVE_N - 1), 78 - v * 38 / gain };
    }
    lv_line_set_points(s_scope, s_scope_pts, WAVE_N);
    lv_obj_set_style_line_color(s_scope, hex(s_muted ? C_RED : (s_level > 40 ? C_CYAN : C_GREEN)), 0);

    if (t % 2 == 0) {                                      /* voiceprint scrolls left */
        memmove(s_hist, s_hist + 1, BARS - 1);
        s_hist[BARS - 1] = s_muted ? 0 : s_level;
    }
    for (int i = 0; i < BARS; i++) {
        int h = 2 + s_hist[i] * 64 / 100;
        lv_obj_set_pos(s_bar[i], 8 + i * 7 + 1, 204 - h);
        lv_obj_set_height(s_bar[i], h);
        uint32_t c = s_hist[i] > 70 ? C_RED : s_hist[i] > 40 ? C_AMBER : C_GREEN;
        lv_obj_set_style_bg_color(s_bar[i], hex(c), 0);
    }
    char b[24];
    snprintf(b, sizeof b, "%d dB", s_muted ? -99 : -60 + s_level * 6 / 10);
    lv_label_set_text(s_db, b);
    lv_label_set_text(s_state, s_muted ? "[ MIC MUTED ]" : (s_level > 40 ? "[ VOICE DETECTED ]" : "[ listening ]"));
    lv_obj_set_style_text_color(s_state, hex(s_muted ? C_RED : (s_level > 40 ? C_CYAN : C_GREEN)), 0);
}

/* ================================================================ chrome */
static const char *const TERM[] = {
    "$ arp-scan --localnet",
    "$ nmap -sV -T4 --top-ports 100 gw",
    "[+] 3 services fingerprinted",
    "$ tcpdump -i wlan0 -c 500 port 53",
    "$ hashcat -m 22000 capture.hc22000",
    "[*] candidates 41.2 kH/s  ETA 03:12",
    "$ ssh -J bastion ops@node-07",
    "[+] tunnel up  aes256-gcm",
    "$ tail -f /var/log/auth.log",
    "[!] intrusion detection: nominal",
};
static int term_idx, term_pos;

static void chrome_build(lv_obj_t *l)
{
    static const int corner[4][2] = { { 2, 14 }, { W - 14, 14 }, { 2, H - 26 }, { W - 14, H - 26 } };
    for (int i = 0; i < 4; i++) {
        int x = corner[i][0], y = corner[i][1];
        box(l, x, y + ((i / 2) ? 11 : 0), 12, 1, C_GREEN, LV_OPA_80);
        box(l, x + ((i % 2) ? 11 : 0), y, 1, 12, C_GREEN, LV_OPA_80);
    }
    lbl_top = txt(l, F8, C_GREEN, 4, 3, "");
    lbl_scene = txt(l, F8, C_MAG, 0, 3, "");
    lbl_term = txt(l, F8, C_GREEN, 6, H - 12, "");
    /* CRT scanlines: one translucent dark line every other row */
    for (int y = 1; y < H; y += 3) box(l, 0, y, W, 1, 0x000000, LV_OPA_30);
}

static void chrome_step(void)
{
    static const char *const names[N_SCENES] = { "RADAR", "GLOBE", "DECRYPT", "SIGNAL" };
    char b[72];
    snprintf(b, sizeof b, "DEEP DIVE // %.12s // CPU %u%% %d.%dC", s_host, s_cpu,
             s_temp_x10 / 10, s_temp_x10 % 10 < 0 ? -(s_temp_x10 % 10) : s_temp_x10 % 10);
    lv_label_set_text(lbl_top, b);
    snprintf(b, sizeof b, "[%d/4 %s]", cur + 1, names[cur]);
    lv_label_set_text(lbl_scene, b);
    lv_obj_align(lbl_scene, LV_ALIGN_TOP_RIGHT, -4, 3);

    /* terminal: type a line, hold it, next */
    const char *line_s = TERM[term_idx];
    int len = (int)strlen(line_s);
    term_pos++;
    if (term_pos > len + 30) {
        term_idx = (term_idx + 1) % (int)(sizeof TERM / sizeof TERM[0]);
        term_pos = 0;
    }
    int shown = term_pos > len ? len : term_pos;
    if (shown > (int)sizeof b - 2) shown = (int)sizeof b - 2;
    memcpy(b, line_s, (size_t)shown);
    b[shown] = (t / 6) % 2 ? '_' : ' ';
    b[shown + 1] = '\0';
    lv_label_set_text(lbl_term, b);
    lv_obj_set_style_text_color(lbl_term, hex(line_s[0] == '[' ? (line_s[1] == '!' ? C_AMBER : C_CYAN) : C_GREEN), 0);
}

/* ================================================================ driver */
static void set_scene(int i)
{
    cur = i;
    for (int k = 0; k < N_SCENES; k++) show(scene[k], k == i);
}

static void glitch_step(void)
{
    static const uint32_t gc[] = { C_CYAN, C_MAG, C_GREEN, C_WHITE };
    for (int i = 0; i < 7; i++) {
        bool on = glitch > 0 && rnd(3);
        show(glitch_bar[i], on);
        if (!on) continue;
        lv_obj_set_pos(glitch_bar[i], rnd(80) - 40, 14 + rnd(H - 30));
        lv_obj_set_size(glitch_bar[i], 120 + rnd(260), 1 + rnd(7));
        lv_obj_set_style_bg_color(glitch_bar[i], hex(gc[rnd(4)]), 0);
    }
    lv_obj_set_style_translate_x(stage, glitch > 0 ? rnd(13) - 6 : 0, 0);
    if (glitch == GLITCH_TICKS / 2) set_scene((cur + 1) % N_SCENES);   /* swap mid-glitch */
    if (glitch > 0) glitch--;
}

static void step(lv_timer_t *tm)
{
    (void)tm;
    t++;
    if (t % SCENE_TICKS == 0) glitch = GLITCH_TICKS;
    else if (rnd(400) == 0 && glitch == 0) glitch = 2;              /* rare micro-glitch */
    switch (cur) {
    case 0: radar_step(); break;
    case 1: globe_step(); break;
    case 2: decrypt_step(); break;
    default: signal_step(); break;
    }
    chrome_step();
    glitch_step();
}

static void hide_cb(lv_event_t *e)
{
    (void)e;
    saver_hide();
}

void saver_create(void)
{
    layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(layer);
    lv_obj_set_size(layer, W, H);
    lv_obj_set_style_bg_color(layer, hex(C_BG), 0);
    lv_obj_set_style_bg_opa(layer, LV_OPA_COVER, 0);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(layer, hide_cb, LV_EVENT_CLICKED, NULL);

    stage = lv_obj_create(layer);
    lv_obj_remove_style_all(stage);
    lv_obj_set_size(stage, W, H);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < N_SCENES; i++) {
        scene[i] = lv_obj_create(stage);
        lv_obj_remove_style_all(scene[i]);
        lv_obj_set_size(scene[i], W, H);
        lv_obj_remove_flag(scene[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    radar_build(scene[0]);
    globe_build(scene[1]);
    decrypt_build(scene[2]);
    signal_build(scene[3]);
    chrome_build(layer);
    for (int i = 0; i < 7; i++) {
        glitch_bar[i] = box(layer, 0, 0, 10, 2, C_CYAN, LV_OPA_80);
        show(glitch_bar[i], false);
    }
    set_scene(0);

    timer = lv_timer_create(step, TICK_MS, NULL);
    lv_timer_pause(timer);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
}

void saver_show(void)
{
    if (!saver_visible()) {
        t = 0;
        glitch = GLITCH_TICKS / 2 + 1;     /* open with a glitch */
        set_scene(N_SCENES - 1);           /* the glitch swaps to scene 0 */
        lv_obj_remove_flag(layer, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(timer);
    }
}

void saver_hide(void)
{
    lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(timer);
}

bool saver_visible(void)
{
    return !lv_obj_has_flag(layer, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------------------------------------------------- data inputs */
void saver_set_identity(const char *host, const char *ip, uint32_t uptime_s, uint8_t cpu, int16_t temp_x10)
{
    snprintf(s_host, sizeof s_host, "%s", host);
    snprintf(s_ip, sizeof s_ip, "%s", ip);
    s_uptime = uptime_s;
    s_cpu = cpu;
    s_temp_x10 = temp_x10;
}

void saver_set_hosts(const uint8_t (*ips)[4], int n)
{
    if (n < 0) n = 0;
    if (n > MAX_HOSTS) n = MAX_HOSTS;
    memcpy(s_hosts, ips, (size_t)n * 4);
    s_nhosts = n;
}

void saver_audio(const int16_t *pcm, int n, uint8_t level_pct, bool muted)
{
    int step = n / WAVE_N;
    if (step < 1) step = 1;
    for (int i = 0; i < WAVE_N && i * step < n; i++) s_wave[i] = pcm[i * step];
    s_level = level_pct;
    s_muted = muted;
}
