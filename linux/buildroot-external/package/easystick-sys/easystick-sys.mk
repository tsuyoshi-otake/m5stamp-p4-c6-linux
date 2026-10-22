################################################################################
# EasyStick system-side task-mark helper
################################################################################

EASYSTICK_SYS_SITE = \
	$(BR2_EXTERNAL_EASYSTICK_STAMP_P4_PATH)/package/easystick-sys/src
EASYSTICK_SYS_SITE_METHOD = local
EASYSTICK_SYS_LICENSE = GPL-2.0-only

define EASYSTICK_SYS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/easystick-sys \
		$(@D)/easystick-sys.c
endef

define EASYSTICK_SYS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/easystick-sys \
		$(TARGET_DIR)/usr/sbin/easystick-sys
endef

$(eval $(generic-package))
