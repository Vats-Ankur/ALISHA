/* LVGL settings for the desktop preview. Mirrors the firmware's sdkconfig LVGL options. */
#pragma once
#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 20
#define LV_DPI_DEF 130
#define LV_DRAW_SW_COMPLEX 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_UNSCII_8 1
#define LV_FONT_UNSCII_16 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_GRADIENT_MAX_STOPS 2
#define LV_USE_LOG 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
