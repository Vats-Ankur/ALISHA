/* DEEP DIVE board screensaver: start/stop it and feed it live mic audio (saver.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* "DEEP DIVE" hacker screensaver. Lifecycle calls need the LVGL lock held.
 * Any button or touch dismisses it. */
void saver_create(void);
void saver_show(void);
void saver_hide(void);
bool saver_visible(void);

/* live data (set_identity / set_hosts: LVGL lock held; saver_audio: any task, lock-free) */
void saver_set_identity(const char *host, const char *ip, uint32_t uptime_s, uint8_t cpu, int16_t temp_x10);
void saver_set_hosts(const uint8_t (*ips)[4], int n);
void saver_audio(const int16_t *pcm, int n, uint8_t level_pct, bool muted);
