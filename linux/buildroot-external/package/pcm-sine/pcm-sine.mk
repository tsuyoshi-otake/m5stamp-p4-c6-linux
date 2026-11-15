################################################################################
# Tab5 audio probe: raw-ioctl sine player (#24)
################################################################################

PCM_SINE_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/pcm-sine/src
PCM_SINE_SITE_METHOD = local
PCM_SINE_LICENSE = GPL-2.0-only

define PCM_SINE_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/pcm-sine $(@D)/pcm-sine.c -lm
endef

define PCM_SINE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pcm-sine \
		$(TARGET_DIR)/usr/bin/pcm-sine
endef

$(eval $(generic-package))
