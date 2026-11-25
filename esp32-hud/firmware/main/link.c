/* Framed, CRC-checked link over UART0 (wired to the CP2102 USB bridge). */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "link.h"
#include "proto.h"

#define LINK_UART   UART_NUM_0
#define LINK_BAUD   921600
#define PIN_TX      43
#define PIN_RX      44

static SemaphoreHandle_t s_tx_mtx;
static uint8_t           s_seq;
static link_frame_cb_t   s_cb;
static volatile int64_t  s_last_rx_us = -1;

uint16_t link_crc16(const uint8_t *d, size_t n, uint16_t crc)
{
    while (n--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

bool link_send(uint8_t type, const void *payload, uint16_t len)
{
    if (len > PROTO_MAX_OUT || (len && !payload)) {
        return false;
    }
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    uint8_t hdr[6] = { PROTO_SOF0, PROTO_SOF1, type, s_seq++, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    uint16_t crc = link_crc16(&hdr[2], 4, 0xFFFF);
    crc = link_crc16(payload, len, crc);
    uint8_t tail[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
    uart_write_bytes(LINK_UART, hdr, sizeof hdr);
    if (len) {
        uart_write_bytes(LINK_UART, payload, len);
    }
    uart_write_bytes(LINK_UART, tail, sizeof tail);
    xSemaphoreGive(s_tx_mtx);
    return true;
}

bool link_host_alive(uint32_t ms)
{
    int64_t last = s_last_rx_us;
    return last >= 0 && (esp_timer_get_time() - last) < (int64_t)ms * 1000;
}

/* Byte-wise state machine. Any malformed frame resyncs on the next SOF. */
static void rx_task(void *arg)
{
    enum { S_SOF0, S_SOF1, S_HDR, S_PAY, S_CRC } st = S_SOF0;
    static uint8_t buf[PROTO_MAX_IN];
    uint8_t hdr[4];
    uint8_t crcb[2];
    size_t  idx = 0;
    uint16_t len = 0;
    uint8_t chunk[128];

    for (;;) {
        int n = uart_read_bytes(LINK_UART, chunk, sizeof chunk, pdMS_TO_TICKS(50));
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            switch (st) {
            case S_SOF0:
                if (b == PROTO_SOF0) st = S_SOF1;
                break;
            case S_SOF1:
                st = (b == PROTO_SOF1) ? S_HDR : (b == PROTO_SOF0 ? S_SOF1 : S_SOF0);
                idx = 0;
                break;
            case S_HDR:
                hdr[idx++] = b;
                if (idx == sizeof hdr) {
                    len = (uint16_t)(hdr[2] | (hdr[3] << 8));
                    idx = 0;
                    if (len > PROTO_MAX_IN) { st = S_SOF0; break; }   /* reject oversize */
                    st = len ? S_PAY : S_CRC;
                }
                break;
            case S_PAY:
                buf[idx++] = b;
                if (idx == len) { idx = 0; st = S_CRC; }
                break;
            case S_CRC:
                crcb[idx++] = b;
                if (idx == 2) {
                    uint16_t want = (uint16_t)(crcb[0] | (crcb[1] << 8));
                    uint16_t got = link_crc16(hdr, sizeof hdr, 0xFFFF);
                    got = link_crc16(buf, len, got);
                    if (want == got) {
                        s_last_rx_us = esp_timer_get_time();
                        if (s_cb) s_cb(hdr[0], buf, len);
                    }
                    st = S_SOF0;
                }
                break;
            }
        }
    }
}

void link_init(link_frame_cb_t cb)
{
    s_cb = cb;
    s_tx_mtx = xSemaphoreCreateMutex();
    const uart_config_t cfg = {
        .baud_rate = LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 2048, 8192, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, PIN_TX, PIN_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreatePinnedToCore(rx_task, "link_rx", 4096, NULL, 7, NULL, 0);
}
