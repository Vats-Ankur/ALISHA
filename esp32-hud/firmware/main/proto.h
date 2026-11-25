/*
 * ALISHA HUD link protocol (ESP32 <-> Raspberry Pi over USB serial).
 *
 * Frame:  A5 5A | type u8 | seq u8 | len u16 LE | payload[len] | crc16 LE
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over type..payload.
 * Every inbound payload is length-checked against the exact size it must have.
 * Keep in sync with host/alisha_hud_bridge.py.
 */
#pragma once
#include <stdint.h>

#define PROTO_VERSION      1
#define PROTO_SOF0         0xA5
#define PROTO_SOF1         0x5A
#define PROTO_MAX_OUT      1024   /* largest payload the device sends   */
#define PROTO_MAX_IN       1100   /* largest payload the device accepts (speaker audio) */
#define PROTO_TEXT_MAX     120    /* heard/reply text                    */
#define PROTO_LOG_MAX      60     /* ops-feed line                       */
#define PROTO_HOSTS_MAX    32     /* LAN hosts shown on the radar        */

/* device -> host */
enum {
    MSG_HELLO  = 0x01,  /* u8 proto_version, char fw[15]              */
    MSG_AUDIO  = 0x02,  /* int16 LE mono PCM, 16 kHz                  */
    MSG_LEVELS = 0x03,  /* u16 rms[4] (raw per ES7210 channel)        */
    MSG_EVENT  = 0x04,  /* u8 code, u8 arg                            */
};

/* host -> device */
enum {
    MSG_STATS  = 0x10,  /* hud_stats_t                                */
    MSG_STATE  = 0x11,  /* u8 alisha_state                            */
    MSG_LOG    = 0x12,  /* u8 level, char text[1..PROTO_LOG_MAX]      */
    MSG_HEARD  = 0x13,  /* char text[0..PROTO_TEXT_MAX]               */
    MSG_REPLY  = 0x14,  /* char text[0..PROTO_TEXT_MAX]               */
    MSG_CONFIG = 0x15,  /* u8 mic_channel (0-3, 0xFF = mix), u8 gain_db */
    MSG_TIME   = 0x16,  /* u32 unix_time, i16 utc_offset_minutes      */
    MSG_PING   = 0x17,  /* empty                                      */
    MSG_SAVER  = 0x18,  /* u8 1 = show screensaver, 0 = hide          */
    MSG_HOSTS  = 0x19,  /* u8 n (0..32), then n x IPv4[4] from the Pi ARP table */
    MSG_SPEAKER = 0x1A, /* int16 LE mono PCM, 16 kHz, even length, <= PROTO_MAX_IN */
};

/* MSG_EVENT codes */
enum {
    EVT_MIC_MUTED   = 1,  /* arg: 1 muted, 0 live */
    EVT_ACTION      = 2,  /* arg: action id below */
    EVT_BOOT        = 3,
};

/* action ids sent from the touch "ACTIONS" page. The host maps these to an
 * allowlist; the device never sends free-form commands. */
enum {
    ACT_STATUS = 1,
    ACT_LOCK   = 2,
    ACT_MATRIX = 3,
    ACT_TIME   = 4,
};

enum {
    JS_IDLE = 0, JS_LISTENING = 1, JS_THINKING = 2, JS_SPEAKING = 3, JS_ASLEEP = 4,
    JS_MAX = JS_ASLEEP,
};

enum { LOG_INFO = 0, LOG_OK = 1, LOG_WARN = 2, LOG_ALERT = 3, LOG_LVL_MAX = LOG_ALERT };

typedef struct __attribute__((packed)) {
    uint8_t  cpu_pct;
    uint8_t  mem_pct;
    uint8_t  disk_pct;
    int16_t  temp_c_x10;
    uint16_t fan_rpm;
    uint32_t rx_bps;
    uint32_t tx_bps;
    uint32_t uptime_s;
    uint16_t lan_hosts;
    uint16_t listen_ports;
    uint8_t  ssh_sessions;
    char     ip[16];
    char     host[16];
} hud_stats_t;

_Static_assert(sizeof(hud_stats_t) == 56, "hud_stats_t layout must match host");
