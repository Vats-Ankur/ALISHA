idf_component_register(
    SRCS "main.c" "board.c" "link.c" "ui.c" "saver.c"
         "font_hack_10.c" "font_hack_12.c" "font_hack_bold_14.c" "font_hack_bold_20.c" "font_hack_bold_34.c" "font_icons_13.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_uart esp_driver_gpio
             esp_lcd esp_timer heap
)
# Hardening: treat warnings as errors in our own code.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror -Wformat=2 -Wformat-truncation=2 -Wshadow -Wvla
    -Wno-missing-field-initializers -Wno-unused-parameter)
