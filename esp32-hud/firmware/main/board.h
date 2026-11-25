/* Board handles and bring-up API: LCD, buttons, ES7210 mic ADC, ES8311 speaker DAC (board.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_codec_dev.h"

/* LCKFB "Shizhanpai" ESP32-S3: ST7789 320x240 + FT5x06 touch, ES7210 4-ch mic ADC,
 * PCA9557 I/O expander (LCD CS, speaker PA enable, camera power-down). */
#define LCD_H_RES        320
#define LCD_V_RES        240
#define MIC_SAMPLE_RATE  16000
#define MIC_CHANNELS     4
#define PIN_BOOT_BUTTON  0
#define PIN_BTN_UP       20
#define PIN_BTN_DOWN     19

typedef struct {
    esp_lcd_panel_io_handle_t lcd_io;
    esp_lcd_panel_handle_t    lcd_panel;
    esp_lcd_touch_handle_t    touch;   /* NULL if the touch controller did not answer */
    esp_codec_dev_handle_t    mic;     /* NULL if the mic ADC failed */
    esp_codec_dev_handle_t    spk;     /* NULL if the speaker DAC failed */
    uint16_t                  spk_id;  /* ES8311 chip id read at boot (0x8311 = genuine) */
} board_t;

esp_err_t board_init(board_t *b);
esp_err_t board_set_mic_gain(board_t *b, float db);
void      board_backlight(bool on);
void      board_pa(bool on);            /* speaker amplifier */
esp_err_t board_set_volume(board_t *b, int vol);
