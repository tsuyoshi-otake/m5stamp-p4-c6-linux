################################################################################
# Stamp-P4 system-side task-mark helper
################################################################################

RUN_SYSTEM_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/run-system/src
RUN_SYSTEM_SITE_METHOD = local
RUN_SYSTEM_LICENSE = GPL-2.0-only

define RUN_SYSTEM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/run-system \
		$(@D)/run-system.c
endef

define RUN_SYSTEM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/run-system \
		$(TARGET_DIR)/usr/sbin/run-system
endef

$(eval $(generic-package))
