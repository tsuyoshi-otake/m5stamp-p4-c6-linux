################################################################################
# EasyStick boot image A/B commit helper
################################################################################

EASYSTICK_BOOT_COMMIT_SITE = \
	$(BR2_EXTERNAL_EASYSTICK_STAMP_P4_PATH)/package/easystick-boot-commit/src
EASYSTICK_BOOT_COMMIT_SITE_METHOD = local
EASYSTICK_BOOT_COMMIT_LICENSE = GPL-2.0-only

define EASYSTICK_BOOT_COMMIT_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/easystick-boot-commit \
		$(@D)/easystick-boot-commit.c
endef

define EASYSTICK_BOOT_COMMIT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/easystick-boot-commit \
		$(TARGET_DIR)/usr/sbin/easystick-boot-commit
endef

$(eval $(generic-package))
