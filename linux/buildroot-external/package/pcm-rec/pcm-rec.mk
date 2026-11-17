################################################################################
# Tab5 audio probe: raw-ioctl recorder (#24/B12)
################################################################################

PCM_REC_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/pcm-rec/src
PCM_REC_SITE_METHOD = local
PCM_REC_LICENSE = GPL-2.0-only

define PCM_REC_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/pcm-rec $(@D)/pcm-rec.c -lm
endef

define PCM_REC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pcm-rec \
		$(TARGET_DIR)/usr/bin/pcm-rec
endef

$(eval $(generic-package))
