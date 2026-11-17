################################################################################
# Tab5 audio probe: raw-ioctl file player (#24/B18)
################################################################################

PCM_PLAY_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/pcm-play/src
PCM_PLAY_SITE_METHOD = local
PCM_PLAY_LICENSE = GPL-2.0-only

define PCM_PLAY_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/pcm-play $(@D)/pcm-play.c -lm
endef

define PCM_PLAY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pcm-play \
		$(TARGET_DIR)/usr/bin/pcm-play
endef

$(eval $(generic-package))
