/* desktop stand-in for esp_lvgl_port.h: the preview is single threaded, so locks always succeed */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
static inline bool lvgl_port_lock(uint32_t ms) { (void)ms; return true; }
static inline void lvgl_port_unlock(void) {}
