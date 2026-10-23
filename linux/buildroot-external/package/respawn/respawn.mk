################################################################################
# EasyStick daemon respawn helper
################################################################################

EASYSTICK_RESPAWN_SITE = \
	$(BR2_EXTERNAL_EASYSTICK_STAMP_P4_PATH)/package/easystick-respawn/src
EASYSTICK_RESPAWN_SITE_METHOD = local
EASYSTICK_RESPAWN_LICENSE = GPL-2.0-only

define EASYSTICK_RESPAWN_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/easystick-respawn \
		$(@D)/easystick-respawn.c
endef

define EASYSTICK_RESPAWN_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/easystick-respawn \
		$(TARGET_DIR)/usr/sbin/easystick-respawn
endef

$(eval $(generic-package))
