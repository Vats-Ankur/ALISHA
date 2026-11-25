/* Desktop preview of the ALISHA HUD: runs the real ui.c/saver.c with LVGL on the Pi and
 * writes screenshots (PPM) of each state/page to ./shots. Build + run with ./build.sh. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "lvgl.h"
#include "ui.h"

#define W 320
#define H 240
static uint16_t fb[W * H];
static int64_t s_now_us;
int64_t esp_timer_get_time(void) { return s_now_us; }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    const uint16_t *p = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++) fb[y * W + x] = *p++;
    lv_display_flush_ready(d);
}

static void run(int ms)
{
    for (int t = 0; t < ms; t += 10) { s_now_us += 10000; lv_tick_inc(10); lv_timer_handler(); }
}

static void shot(const char *name)
{
    run(30);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    char path[128];
    snprintf(path, sizeof path, "shots/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t c = fb[i];
        unsigned char rgb[3] = { (unsigned char)(((c >> 11) & 31) * 255 / 31),
                                 (unsigned char)(((c >> 5) & 63) * 255 / 63),
                                 (unsigned char)((c & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("shot %s\n", name);
}

static void txt(void (*fn)(const char *, size_t), const char *s) { fn(s, strlen(s)); }

static void level_wave(int ms, int base)
{
    for (int t = 0; t < ms; t += 100) { ui_set_level((uint8_t)(base + (t * 37 % 40))); run(100); }
}

int main(void)
{
    mkdir("shots", 0755);
    lv_init();
    lv_display_t *d = lv_display_create(W, H);
    static uint16_t buf[W * 40];
    lv_display_set_buffers(d, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);

    ui_init(NULL, NULL);
    run(1300);
    shot("00_boot");
    run(2300);
    ui_set_mic(false, true);
    ui_set_link(true);
    ui_set_time(1795488000u + 23 * 3600 + 47 * 60, 0);
    hud_stats_t st = { 0 };
    st.cpu_pct = 23; st.temp_c_x10 = 512; st.mem_pct = 41; st.uptime_s = 93784; st.fan_rpm = 1800;
    st.disk_pct = 4; st.lan_hosts = 7; st.listen_ports = 3; st.rx_bps = 154000; st.tx_bps = 23000;
    memcpy(st.ip, "192.168.1.150", 13); memcpy(st.host, "kali-raspberrypi", 16);
    for (int i = 0; i < 20; i++) { ui_set_stats(&st); run(50); }

    ui_set_state(JS_IDLE); run(300);
    shot("01_idle");

    ui_set_state(JS_LISTENING); level_wave(1500, 40);
    shot("02_listening");

    txt(ui_set_heard, "Alisha, what is the system status and how hot is the CPU running right now?");
    ui_set_state(JS_THINKING); run(180);
    shot("03a_decrypt");
    run(600);
    shot("03_thinking");

    txt(ui_set_reply, "All systems nominal. CPU load 0.42. Temperature 51.2 degrees. Memory at 41 percent. "
                      "Seven hosts on the network, no SSH sessions.");
    ui_set_state(JS_SPEAKING); level_wave(400, 10);
    shot("04a_typing");
    level_wave(1200, 10);
    shot("04_speaking");

    ui_set_mic(true, true); run(200);
    shot("05_muted");
    ui_set_mic(false, true);
    ui_set_state(JS_IDLE);

    const char *pages[] = { "06_sys", "07_net", "08_ops", "09_keys" };
    for (int i = 0; i < 4; i++) {
        if (i == 2) { const char *l = "SSH login kali from 192.168.1.182"; ui_log(1, l, strlen(l)); }
        ui_page_step(+1); run(600); shot(pages[i]);
    }
    return 0;
}
