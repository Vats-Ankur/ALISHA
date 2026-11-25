/* ALISHA HUD firmware: USB-serial microphone + touch HUD for the Raspberry Pi.
 *
 * Security properties (see ../SECURITY.md):
 *  - no Wi-Fi/Bluetooth code is linked; the only interface is the USB serial link
 *  - no secrets are stored on the device; logs never go to the serial port
 *  - audio is only sent while a host is actively talking to us, and never while muted
 *  - mute is enforced here (BOOT button or touch), the host cannot unmute remotely
 *  - every inbound frame is CRC-checked and length-checked against its exact size
 */
#include <math.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "link.h"
#include "proto.h"
#include "ui.h"
#include "saver.h"

#define FW_VERSION      "hud-0.4.1"
#define AUDIO_FRAMES    320            /* 20 ms at 16 kHz */
#define HOST_TIMEOUT_MS 3000

static board_t     s_board;
static atomic_bool s_muted;
static atomic_int  s_mic_ch = 0;       /* 0..3, or -1 = mix of all four */

/* ---- speaker: Pi -> board audio, played through the ES8311 */
#define SPK_BUF_BYTES   (MIC_SAMPLE_RATE * 2)      /* 1 s of 16 kHz mono */
#define SPK_IDLE_MS     300                        /* amp off after this much silence */
#define ECHO_GUARD_MS   400                        /* mic stays quiet this long after playback */
static StreamBufferHandle_t s_spk_sb;
static _Atomic int64_t      s_spk_quiet_until_us;  /* echo guard deadline */

static bool speaker_busy(void)
{
#ifdef SPK_LOOPBACK_TEST
    return false;                     /* diagnostic build: let the mic hear the speaker */
#else
    return esp_timer_get_time() < atomic_load(&s_spk_quiet_until_us);
#endif
}

/* Playback with a jitter buffer. Audio arrives over USB in uneven bursts; playing each
 * 20 ms chunk the moment it lands made the voice choppy ("robotic"). Instead we wait until
 * SPK_PREBUF_MS is queued (or the sender has gone quiet, for very short clips), then play
 * steadily. If the buffer ever runs dry mid-sentence we pause and refill the same way. */
#define SPK_PREBUF_MS   240
#define SPK_CHUNK_B     (AUDIO_FRAMES * 2)                 /* 20 ms of 16 kHz mono */
#define SPK_PREBUF_B    (MIC_SAMPLE_RATE * 2 * SPK_PREBUF_MS / 1000)

static void speaker_task(void *arg)
{
    static int16_t buf[AUDIO_FRAMES];
    static const int16_t zeros[AUDIO_FRAMES];
    bool playing = false, amp_on = false;
    int64_t last_rx_us = 0, last_play_us = 0;
    size_t last_avail = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        size_t avail = xStreamBufferBytesAvailable(s_spk_sb);
        if (avail != last_avail) { if (avail > last_avail) last_rx_us = now; last_avail = avail; }

        if (!playing) {
            /* start once enough is queued, or once a short clip has fully arrived */
            bool ready = avail >= SPK_PREBUF_B || (avail >= 2 && now - last_rx_us > 120000);
            if (!ready) {
                if (amp_on && now - last_play_us > (int64_t)SPK_IDLE_MS * 1000) {
                    board_pa(false);                       /* amplifier off when idle */
                    amp_on = false;
                    atomic_store(&s_spk_quiet_until_us, now + (int64_t)ECHO_GUARD_MS * 1000);
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (!amp_on) {
                board_pa(true);
                amp_on = true;
                for (int i = 0; i < 3; i++) esp_codec_dev_write(s_board.spk, (void *)zeros, sizeof zeros);  /* let the amp settle */
            }
            playing = true;
        }

        size_t n = xStreamBufferReceive(s_spk_sb, buf, sizeof buf, 0);
        now = esp_timer_get_time();
        if (n >= 2) {
            n &= ~(size_t)1;
            atomic_store(&s_spk_quiet_until_us, now + (int64_t)ECHO_GUARD_MS * 1000 + 1000000);
            esp_codec_dev_write(s_board.spk, buf, (int)n);   /* blocks at real-time pace */
            last_play_us = esp_timer_get_time();
            last_avail = xStreamBufferBytesAvailable(s_spk_sb);
        } else {
            playing = false;                                /* ran dry: refill before resuming */
            last_play_us = now;
        }
    }
}

static void send_event(uint8_t code, uint8_t arg)
{
    uint8_t p[2] = { code, arg };
    link_send(MSG_EVENT, p, sizeof p);
}

static void toggle_mute(void)
{
    bool m = !atomic_load(&s_muted);
    atomic_store(&s_muted, m);
    ui_set_mic(m, s_board.mic != NULL);
    send_event(EVT_MIC_MUTED, m);
}

static void on_action(uint8_t id)
{
    send_event(EVT_ACTION, id);
}

static void on_frame(uint8_t type, const uint8_t *p, uint16_t len)
{
    switch (type) {
    case MSG_STATS:
        if (len == sizeof(hud_stats_t)) {
            hud_stats_t s;
            memcpy(&s, p, sizeof s);
            ui_set_stats(&s);
        }
        break;
    case MSG_STATE:
        if (len == 1 && p[0] <= JS_MAX) ui_set_state(p[0]);
        break;
    case MSG_LOG:
        if (len >= 2 && len <= 1 + PROTO_LOG_MAX) ui_log(p[0], (const char *)p + 1, len - 1u);
        break;
    case MSG_HEARD:
        if (len <= PROTO_TEXT_MAX) ui_set_heard((const char *)p, len);
        break;
    case MSG_REPLY:
        if (len <= PROTO_TEXT_MAX) ui_set_reply((const char *)p, len);
        break;
    case MSG_CONFIG:
        if (len == 2 && (p[0] <= 3 || p[0] == 0xFF) && p[1] <= 42) {
            atomic_store(&s_mic_ch, p[0] == 0xFF ? -1 : p[0]);
            board_set_mic_gain(&s_board, (float)p[1]);
        }
        break;
    case MSG_TIME:
        if (len == 6) {
            uint32_t t;
            int16_t tz;
            memcpy(&t, p, 4);
            memcpy(&tz, p + 4, 2);
            if (tz >= -14 * 60 && tz <= 14 * 60) ui_set_time(t, tz);
        }
        break;
    case MSG_HOSTS:
        if (len >= 1 && p[0] <= PROTO_HOSTS_MAX && len == 1u + 4u * p[0]) {
            uint8_t ips[PROTO_HOSTS_MAX][4];
            memcpy(ips, p + 1, 4u * p[0]);
            ui_set_hosts((const uint8_t (*)[4])ips, p[0]);
        }
        break;
    case MSG_SPEAKER:
        /* never block the link: if the 1 s buffer is full the extra audio is dropped */
        if (s_spk_sb && len >= 2 && (len % 2) == 0) xStreamBufferSend(s_spk_sb, p, len, 0);
        break;
    case MSG_SAVER:
        if (len == 1 && p[0] <= 1) ui_saver(p[0]);
        break;
    case MSG_PING:
    default:
        break;   /* unknown types are ignored */
    }
}

static void audio_task(void *arg)
{
    static int16_t raw[AUDIO_FRAMES * MIC_CHANNELS];
    static int16_t mono[AUDIO_FRAMES];
    uint64_t acc[MIC_CHANNELS] = { 0 };
    uint32_t acc_n = 0;
    int btn_prev = 1, btn_stable = 0;
    /* UP/DOWN: short press = page, hold 1 s = action (UP: status, DOWN: DEEP DIVE saver) */
    const int nav_pin[2] = { PIN_BTN_UP, PIN_BTN_DOWN };
    int nav_held[2] = { 0, 0 };
    bool link_prev = false;
    uint8_t last_pct = 0;

    for (;;) {
        /* BOOT button = hardware mute toggle (debounced, active low) */
        int btn = gpio_get_level(PIN_BOOT_BUTTON);
        if (btn == btn_prev) {
            if (++btn_stable == 3 && btn == 0) toggle_mute();
        } else {
            btn_stable = 0;
            btn_prev = btn;
        }

        for (int k = 0; k < 2; k++) {
            if (gpio_get_level(nav_pin[k]) == 0) {
                if (++nav_held[k] == 50) {                 /* ~1 s held */
                    if (k == 0) on_action(ACT_STATUS);
                    else        ui_saver(true);
                }
            } else {
                if (nav_held[k] >= 2 && nav_held[k] < 50) ui_page_step(k == 0 ? -1 : +1);
                nav_held[k] = 0;
            }
        }

        bool alive = link_host_alive(HOST_TIMEOUT_MS);
        if (alive != link_prev) {
            ui_set_link(alive);
            if (alive) {
                uint8_t hello[16] = { PROTO_VERSION };
                strncpy((char *)hello + 1, FW_VERSION, 10);
                /* bytes 12-13: speaker chip id (0x8311 = ES8311 found); byte 14: flags */
                hello[12] = (uint8_t)(s_board.spk_id >> 8);
                hello[13] = (uint8_t)(s_board.spk_id & 0xFF);
                hello[14] = (s_board.mic ? 1 : 0) | (s_board.spk ? 2 : 0);
                link_send(MSG_HELLO, hello, sizeof hello);
                send_event(EVT_MIC_MUTED, atomic_load(&s_muted));
            }
            link_prev = alive;
        }

        if (!s_board.mic) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (esp_codec_dev_read(s_board.mic, raw, sizeof raw) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int ch = atomic_load(&s_mic_ch);
        for (int i = 0; i < AUDIO_FRAMES; i++) {
            const int16_t *f = &raw[i * MIC_CHANNELS];
            int32_t mix = 0;
            for (int c = 0; c < MIC_CHANNELS; c++) {
                acc[c] += (uint64_t)((int32_t)f[c] * f[c]);
                mix += f[c];
            }
            mono[i] = (ch < 0) ? (int16_t)(mix / MIC_CHANNELS) : f[ch];
        }
        acc_n += AUDIO_FRAMES;

        if (acc_n >= MIC_SAMPLE_RATE / 10) {           /* every 100 ms */
            uint16_t rms[MIC_CHANNELS];
            for (int c = 0; c < MIC_CHANNELS; c++) {
                rms[c] = (uint16_t)sqrtf((float)acc[c] / (float)acc_n);
                acc[c] = 0;
            }
            acc_n = 0;
            if (alive) link_send(MSG_LEVELS, rms, sizeof rms);
            uint32_t sel = (ch < 0) ? (uint32_t)(rms[0] + rms[1] + rms[2] + rms[3]) / 4u : rms[ch];
            /* map roughly logarithmically to 0..100 for the reactor ring */
            float db = 20.0f * log10f((float)sel + 1.0f);  /* 0..~90 */
            int pct = (int)((db - 30.0f) * 2.2f);
            last_pct = atomic_load(&s_muted) ? 0 : (uint8_t)(pct < 0 ? 0 : pct > 100 ? 100 : pct);
            ui_set_level(last_pct);
        }

        saver_audio(mono, AUDIO_FRAMES, last_pct, atomic_load(&s_muted));   /* screensaver scope */

        /* echo guard: while Alisha is talking through the speaker, the mic stays quiet */
        if (alive && !atomic_load(&s_muted) && !speaker_busy()) {
            link_send(MSG_AUDIO, mono, sizeof mono);
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(board_init(&s_board));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_board.lcd_io,
        .panel_handle = s_board.lcd_panel,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = true, .mirror_x = true, .mirror_y = false },
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (s_board.touch) {
        const lvgl_port_touch_cfg_t tcfg = { .disp = disp, .handle = s_board.touch };
        lvgl_port_add_touch(&tcfg);
    }

    lvgl_port_lock(0);
    ui_init(on_action, toggle_mute);
    lvgl_port_unlock();
    ui_set_mic(false, s_board.mic != NULL);
    ui_log(LOG_INFO, "HUD online, waiting for host", 28);
    board_backlight(true);

    link_init(on_frame);
    send_event(EVT_BOOT, 0);
    xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 6, NULL, 1);
    if (s_board.spk) {
        s_spk_sb = xStreamBufferCreateWithCaps(SPK_BUF_BYTES, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_spk_sb) xTaskCreatePinnedToCore(speaker_task, "speaker", 4096, NULL, 6, NULL, 1);
    }
}
