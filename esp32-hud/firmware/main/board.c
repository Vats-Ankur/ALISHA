/* Board bring-up. Pin map taken from the board's boot log and the open-source
 * xiaozhi-esp32 "lckfb/szpi-esp32s3" board definition (they agree). */
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"

#define TAG "board"

#define PIN_I2C_SDA   1
#define PIN_I2C_SCL   2
#define PIN_I2S_MCLK  38
#define PIN_I2S_BCLK  14
#define PIN_I2S_WS    13
#define PIN_I2S_DIN   12
#define PIN_LCD_MOSI  40
#define PIN_LCD_CLK   41
#define PIN_LCD_DC    39
#define PIN_LCD_BL    42      /* active low with PCA9557, active high on the direct-GPIO revision */
/* board revision without the PCA9557 expander drives these directly */
#define PIN_LCD_CS_DIRECT  47
#define PIN_PA_DIRECT      48

#define PCA9557_ADDR  0x19
#define PCA_REG_OUT   0x01
#define PCA_REG_CFG   0x03
#define PCA_LCD_CS    (1u << 0)   /* active low   */
#define PCA_PA_EN     (1u << 1)   /* speaker amp  */
#define PCA_CAM_PWDN  (1u << 2)   /* high = camera off */

#define ES7210_ADDR   0x82        /* 8-bit form, as esp_codec_dev expects */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_pca;
static uint8_t                 s_pca_out;
static bool                    s_has_pca;

void board_backlight(bool on);

static esp_err_t pca_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_pca, b, sizeof b, 100);
}

static esp_err_t pca_set(uint8_t mask, bool high)
{
    if (!s_has_pca) return ESP_OK;
    s_pca_out = high ? (s_pca_out | mask) : (s_pca_out & (uint8_t)~mask);
    return pca_write(PCA_REG_OUT, s_pca_out);
}

static esp_err_t init_i2c(void)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c), TAG, "i2c bus");

    /* log what answers on the bus (debug builds only print this) */
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(s_i2c, a, 20) == ESP_OK) ESP_LOGI(TAG, "i2c device at 0x%02x", a);
    }

    s_has_pca = (i2c_master_probe(s_i2c, PCA9557_ADDR, 50) == ESP_OK);
    if (!s_has_pca) {
        /* direct-GPIO revision: speaker amp off until we use it */
        const gpio_config_t pa = { .pin_bit_mask = 1ULL << PIN_PA_DIRECT, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&pa);
        gpio_set_level(PIN_PA_DIRECT, 0);
        ESP_LOGI(TAG, "no PCA9557: using direct LCD CS / PA pins");
        return ESP_OK;
    }
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_pca), TAG, "pca9557");
    /* LCD deselected, speaker amp off, camera powered down; IO0-2 outputs. */
    s_pca_out = PCA_LCD_CS | PCA_CAM_PWDN;
    ESP_RETURN_ON_ERROR(pca_write(PCA_REG_OUT, s_pca_out), TAG, "pca out");
    ESP_RETURN_ON_ERROR(pca_write(PCA_REG_CFG, 0xF8), TAG, "pca cfg");
    return ESP_OK;
}

static esp_err_t init_lcd(board_t *b)
{
    gpio_config_t bl = { .pin_bit_mask = 1ULL << PIN_LCD_BL, .mode = GPIO_MODE_OUTPUT };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "bl gpio");
    board_backlight(false);          /* off while initialising */

    const spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi");

    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = s_has_pca ? -1 : PIN_LCD_CS_DIRECT,   /* PCA9557 or direct pin */
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 2,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io, &b->lcd_io), TAG, "lcd io");

    const esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(b->lcd_io, &pc, &b->lcd_panel), TAG, "st7789");
    esp_lcd_panel_reset(b->lcd_panel);
    ESP_RETURN_ON_ERROR(pca_set(PCA_LCD_CS, false), TAG, "lcd cs");
    esp_lcd_panel_init(b->lcd_panel);
    esp_lcd_panel_invert_color(b->lcd_panel, true);
    esp_lcd_panel_swap_xy(b->lcd_panel, true);
    esp_lcd_panel_mirror(b->lcd_panel, true, false);
    esp_lcd_panel_disp_on_off(b->lcd_panel, true);
    return ESP_OK;
}

void board_backlight(bool on)
{
    bool level = s_has_pca ? !on : on;
    gpio_set_level(PIN_LCD_BL, level ? 1 : 0);
}

static esp_err_t init_touch(board_t *b)
{
    if (i2c_master_probe(s_i2c, 0x38, 50) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;    /* this board revision has buttons, not touch */
    }
    esp_lcd_panel_io_handle_t tio = NULL;
    esp_lcd_panel_io_i2c_config_t tio_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tio_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &tio_cfg, &tio), TAG, "touch io");

    const esp_lcd_touch_config_t tc = {
        .x_max = LCD_V_RES,
        .y_max = LCD_H_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .flags = { .swap_xy = 1, .mirror_x = 1, .mirror_y = 0 },
    };
    return esp_lcd_touch_new_i2c_ft5x06(tio, &tc, &b->touch);
}

#define PIN_I2S_DOUT  45
#define ES8311_I2C7   0x18

void board_pa(bool on)
{
    if (s_has_pca) pca_set(PCA_PA_EN, on);
    else           gpio_set_level(PIN_PA_DIRECT, on ? 1 : 0);
}

/* read the ES8311's chip-id registers: a genuine part answers 0x83, 0x11 */
static uint16_t es8311_chip_id(void)
{
    i2c_master_dev_handle_t dev;
    const i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ES8311_I2C7,
                                      .scl_speed_hz = 100000 };
    if (i2c_master_bus_add_device(s_i2c, &cfg, &dev) != ESP_OK) return 0;
    uint8_t reg, hi = 0, lo = 0;
    reg = 0xFD; i2c_master_transmit_receive(dev, &reg, 1, &hi, 1, 100);
    reg = 0xFE; i2c_master_transmit_receive(dev, &reg, 1, &lo, 1, 100);
    i2c_master_bus_rm_device(dev);
    return (uint16_t)((hi << 8) | lo);
}

/* One I2S port in full duplex (as this board family is wired): speaker out in standard
 * I2S to the ES8311, 4-slot TDM in from the ES7210 mics; MCLK/BCLK/WS are shared. */
static esp_err_t init_audio(board_t *b)
{
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 320;
    chan.auto_clear_after_cb = true;          /* underrun plays silence, not a buzz */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &tx, &rx), TAG, "i2s chan");

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT, .din = I2S_GPIO_UNUSED,
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std), TAG, "i2s std");

    i2s_tdm_config_t tdm = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED, .din = PIN_I2S_DIN,
        },
    };
    tdm.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(rx, &tdm), TAG, "i2s tdm");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data, ESP_FAIL, TAG, "i2s data if");

    /* ---- speaker: ES8311 DAC; the amplifier is switched by board_pa() */
    b->spk_id = es8311_chip_id();
    audio_codec_i2c_cfg_t o_i2c = { .port = I2C_NUM_1, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *octrl = audio_codec_new_i2c_ctrl(&o_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (octrl && gpio_if) {
        es8311_codec_cfg_t es8311 = {
            .ctrl_if = octrl,
            .gpio_if = gpio_if,
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
            .pa_pin = -1,
            .use_mclk = true,
            .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
        };
        const audio_codec_if_t *ocif = es8311_codec_new(&es8311);
        if (ocif) {
            esp_codec_dev_cfg_t odc = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = ocif, .data_if = data };
            b->spk = esp_codec_dev_new(&odc);
            esp_codec_dev_sample_info_t ofs = { .bits_per_sample = 16, .channel = 1,
                                                .sample_rate = MIC_SAMPLE_RATE, .mclk_multiple = 256 };
            if (b->spk && esp_codec_dev_open(b->spk, &ofs) == ESP_CODEC_DEV_OK) {
                esp_codec_dev_set_out_vol(b->spk, 75);
            } else {
                b->spk = NULL;
            }
        }
    }

    /* ---- microphones: ES7210 ADC, 4 channels over TDM */
    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_1, .addr = ES7210_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl, ESP_FAIL, TAG, "mic ctrl if");
    es7210_codec_cfg_t es = {
        .ctrl_if = ctrl,
        .mic_selected = ES7120_SEL_MIC1 | ES7120_SEL_MIC2 | ES7120_SEL_MIC3 | ES7120_SEL_MIC4,
    };
    const audio_codec_if_t *cif = es7210_codec_new(&es);
    ESP_RETURN_ON_FALSE(cif, ESP_FAIL, TAG, "es7210");
    esp_codec_dev_cfg_t dc = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = cif, .data_if = data };
    b->mic = esp_codec_dev_new(&dc);
    ESP_RETURN_ON_FALSE(b->mic, ESP_FAIL, TAG, "codec dev");
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = MIC_CHANNELS,
        .channel_mask = 0x0F,
        .sample_rate = MIC_SAMPLE_RATE,
        .mclk_multiple = 256,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(b->mic, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "mic open");
    return board_set_mic_gain(b, 30.0f);
}

esp_err_t board_set_volume(board_t *b, int vol)
{
    if (!b->spk) return ESP_ERR_INVALID_STATE;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    return esp_codec_dev_set_out_vol(b->spk, vol) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t board_set_mic_gain(board_t *b, float db)
{
    if (!b->mic) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_set_in_gain(b->mic, db) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t board_init(board_t *b)
{
    memset(b, 0, sizeof *b);
    const gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BOOT_BUTTON) | (1ULL << PIN_BTN_UP) | (1ULL << PIN_BTN_DOWN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(init_lcd(b), TAG, "lcd");
    if (init_touch(b) != ESP_OK) {
        b->touch = NULL;           /* HUD still works without touch */
    }
    if (init_audio(b) != ESP_OK) {
        b->mic = NULL;             /* HUD still works without mic */
    }
    return ESP_OK;
}
