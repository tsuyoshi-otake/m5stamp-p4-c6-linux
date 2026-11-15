################################################################################
# Tab5 touch demo: evdev in, dots on /dev/fb0 (#33)
################################################################################

TOUCH_DOTS_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/touch-dots/src
TOUCH_DOTS_SITE_METHOD = local
TOUCH_DOTS_LICENSE = GPL-2.0-only

define TOUCH_DOTS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/touch-dots $(@D)/touch-dots.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/touch-cal $(@D)/touch-cal.c -lm
endef

define TOUCH_DOTS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/touch-dots \
		$(TARGET_DIR)/usr/bin/touch-dots
	$(INSTALL) -D -m 0755 $(@D)/touch-cal \
		$(TARGET_DIR)/usr/bin/touch-cal
endef

$(eval $(generic-package))
