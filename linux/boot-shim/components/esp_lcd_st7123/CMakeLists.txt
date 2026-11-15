# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
#
# Vendored from the M5Stack Tab5 UserDemo (Apache-2.0, Espressif).  Only the
# ST7123 panel driver and the BSP's ST7123 vendor init data are carried here;
# the ST7121 driver is the sibling esp_lcd_st7121 component and the ILI9881C
# path of the stock BSP is not carried at all.
#
# NOTE: the unit this port was brought up on is an ST7121 (stock boot log:
# "Detected ST7121 touch controller (FW version: 1), using ST7121 display").
# The "ST7123 touch firmware 1.80.1.16" in that same log is only the name of
# the touch driver, which both chips share.  Nothing in this component has
# run on an ST7123 unit yet; it is the stock BSP's other branch, carried so
# that tab5_display.c can choose by the touch firmware byte like the BSP.
#
# The component compiles into nothing unless the shim is built for Tab5, so
# the proven M1/M2/C68 shim images keep their exact dependency set.
# Provenance: esp_lcd_st7123.[ch] are byte-identical to
# m5stack/M5Tab5-UserDemo @ fe8aa051beb9385468f7e46b909e764169df3f7a
# (platforms/tab5/components/m5stack_tab5, Apache-2.0) and
# disp_init_data_st7123.h to espressif/esp-bsp @
# 67f41a169401d1f23e827188dd08fb351cb0febe
# (bsp/m5stack_tab5/priv_include/disp_init_data_1.h).  The vendored copies
# here are the source of truth for this build.
if("$ENV{ES_BOARD}" STREQUAL "tab5")
    idf_component_register(
        SRCS "esp_lcd_st7123.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_lcd driver
    )
else()
    idf_component_register()
endif()
