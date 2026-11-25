/* USB-serial framed link to the Raspberry Pi: send frames, receive them through a callback (link.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef void (*link_frame_cb_t)(uint8_t type, const uint8_t *payload, uint16_t len);

void     link_init(link_frame_cb_t cb);
bool     link_send(uint8_t type, const void *payload, uint16_t len);
/* true if a valid frame arrived from the host within the last `ms` milliseconds */
bool     link_host_alive(uint32_t ms);
uint16_t link_crc16(const uint8_t *data, size_t len, uint16_t crc);
