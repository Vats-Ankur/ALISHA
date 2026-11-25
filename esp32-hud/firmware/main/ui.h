/* HUD pages (CORE, SYS, NET, OPS, KEYS) and the setters the link uses to update them (ui.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "proto.h"

typedef void (*ui_action_cb_t)(uint8_t action_id);
typedef void (*ui_mute_cb_t)(void);

/* ui_init must be called with the LVGL lock held. All other functions take the lock
 * themselves and may be called from any task. Text is sanitised to printable ASCII. */
void ui_init(ui_action_cb_t on_action, ui_mute_cb_t on_mute_toggle);
void ui_set_stats(const hud_stats_t *s);
void ui_set_state(uint8_t js);
void ui_log(uint8_t level, const char *text, size_t len);
void ui_set_heard(const char *text, size_t len);
void ui_set_reply(const char *text, size_t len);
void ui_set_level(uint8_t pct);
void ui_set_link(bool up);
void ui_set_mic(bool muted, bool present);
void ui_set_time(uint32_t unix_time, int16_t utc_offset_min);
void ui_saver(bool on);
void ui_set_hosts(const uint8_t (*ips)[4], int n);
/* physical-button navigation: dir = -1 / +1 page; returns false if it only woke the screen */
bool ui_page_step(int dir);
