################################################################################
# Stamp-P4 boot image A/B commit helper
################################################################################

BOOT_COMMIT_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/boot-commit/src
BOOT_COMMIT_SITE_METHOD = local
BOOT_COMMIT_LICENSE = GPL-2.0-only

define BOOT_COMMIT_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/boot-commit \
		$(@D)/boot-commit.c
endef

define BOOT_COMMIT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/boot-commit \
		$(TARGET_DIR)/usr/sbin/boot-commit
endef

$(eval $(generic-package))
