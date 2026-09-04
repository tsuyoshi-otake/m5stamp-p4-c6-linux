################################################################################
# M5Stamp C68 L3 SMP smoke helper
################################################################################

M5STAMP_SMP_SMOKE_SITE = \
	$(BR2_EXTERNAL_EASYSTICK_STAMP_P4_PATH)/package/m5stamp-smp-smoke/src
M5STAMP_SMP_SMOKE_SITE_METHOD = local
M5STAMP_SMP_SMOKE_LICENSE = GPL-2.0-only

define M5STAMP_SMP_SMOKE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/m5stamp-smp-smoke $(@D)/m5stamp-smp-smoke.c
endef

define M5STAMP_SMP_SMOKE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/m5stamp-smp-smoke \
		$(TARGET_DIR)/usr/sbin/m5stamp-smp-smoke
endef

$(eval $(generic-package))
