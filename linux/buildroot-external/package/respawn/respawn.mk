################################################################################
# Stamp-P4 daemon respawn helper
################################################################################

RESPAWN_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/respawn/src
RESPAWN_SITE_METHOD = local
RESPAWN_LICENSE = GPL-2.0-only

define RESPAWN_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/respawn \
		$(@D)/respawn.c
endef

define RESPAWN_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/respawn \
		$(TARGET_DIR)/usr/sbin/respawn
endef

$(eval $(generic-package))
