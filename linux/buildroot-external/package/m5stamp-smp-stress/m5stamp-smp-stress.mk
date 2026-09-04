################################################################################
# M5Stamp C68 bounded SMP/process stress helper
################################################################################

M5STAMP_SMP_STRESS_SITE = \
	$(BR2_EXTERNAL_EASYSTICK_STAMP_P4_PATH)/package/m5stamp-smp-stress/src
M5STAMP_SMP_STRESS_SITE_METHOD = local
M5STAMP_SMP_STRESS_LICENSE = GPL-2.0-only

define M5STAMP_SMP_STRESS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 $(TARGET_LDFLAGS) \
		-o $(@D)/m5stamp-smp-stress $(@D)/m5stamp-smp-stress.c
endef

define M5STAMP_SMP_STRESS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/m5stamp-smp-stress \
		$(TARGET_DIR)/usr/sbin/m5stamp-smp-stress
endef

$(eval $(generic-package))
