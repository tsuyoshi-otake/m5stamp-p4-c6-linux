# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
#
# Vendored from the M5Stack Tab5 UserDemo (Apache-2.0, Espressif).  Only the
# ST7121 panel driver is carried; its default vendor init table is the one
# the stock firmware uses (the BSP passes init_cmds = NULL for this panel).
#
# Tab5 units ship with one of two display+touch chips.  The stock BSP tells
# them apart by the touch controller's firmware byte (bsp_detect_display_type:
# 0x55, register 0x0000, 1 = ST7121, 3 = ST7123).  The unit this port was
# brought up on is an ST7121: its stock boot log says "Detected ST7121 touch
# controller (FW version: 1), using ST7121 display" and "Install LCD driver of
# ST7121".  tab5_display.c makes the same choice at run time and uses this
# driver for it and the esp_lcd_st7123 component for the other.
#
# The component compiles into nothing unless the shim is built for Tab5, so
# the proven M1/M2/C68 shim images keep their exact dependency set.
# Provenance: esp_lcd_st7121.[ch] are byte-identical to
# m5stack/M5Tab5-UserDemo @ fe8aa051beb9385468f7e46b909e764169df3f7a
# (platforms/tab5/components/esp_lcd_st7121, Apache-2.0).  The upstream
# CMakeLists.txt only adds the IDF component-manager version hook
# (include(package_manager) / cu_pkg_define_version), which this build does
# not have; everything else here is the same source list.  The vendored
# copies here are the source of truth for this build.
if("$ENV{ES_BOARD}" STREQUAL "tab5")
    idf_component_register(
        SRCS "esp_lcd_st7121.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_lcd driver
    )
else()
    idf_component_register()
endif()
