################################################################################
# m5-key: tty+touch one-shot event reader for the m5-config touch menus (#42)
################################################################################

M5_KEY_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/m5-key/src
M5_KEY_SITE_METHOD = local
M5_KEY_LICENSE = GPL-2.0-only

define M5_KEY_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/m5-key $(@D)/m5-key.c
endef

define M5_KEY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/m5-key \
		$(TARGET_DIR)/usr/bin/m5-key
endef

$(eval $(generic-package))
