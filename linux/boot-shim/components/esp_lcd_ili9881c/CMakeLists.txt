# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
#
# Vendored from esp-bsp components/lcd/esp_lcd_ili9881c (Apache-2.0,
# Espressif).  Carried for the alternate Tab5 panel SKU that pairs an
# ILI9881C MIPI-DSI display with a GT911 touch controller instead of the
# ST7121/ST7123 panel covered by esp_lcd_st7121/esp_lcd_st7123.  The
# driver's built-in vendor_specific_init_default table is used
# (init_cmds = NULL), mirroring how the ST7121 path relies on its
# driver's built-in table.
#
# Provenance: esp_lcd_ili9881c.[ch] fetched from
# espressif/esp-bsp @ master (components/lcd/esp_lcd_ili9881c).  The
# upstream CMakeLists.txt only adds the IDF component-manager version
# hook this build does not have; the source list is the same.  The
# vendored copies here are the source of truth for this build.
if("$ENV{ES_BOARD}" STREQUAL "tab5")
    idf_component_register(
        SRCS "esp_lcd_ili9881c.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_lcd driver
    )
else()
    idf_component_register()
endif()
