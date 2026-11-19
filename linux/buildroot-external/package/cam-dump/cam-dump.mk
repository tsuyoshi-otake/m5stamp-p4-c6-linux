################################################################################
# cam-dump: one-shot /dev/cam0 raw-frame capture tool (Tab5 camera)
################################################################################

CAM_DUMP_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/cam-dump/src
CAM_DUMP_SITE_METHOD = local
CAM_DUMP_LICENSE = GPL-2.0-only

# -elf2flt=-s16384: NOMMU has no stack guard pages and the m3-lab
# post-build gate fails any bFLT below BusyBox's 16000-byte floor
# (#18); the default 4 KB would trip it.
define CAM_DUMP_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-Wl,-elf2flt=-s16384 \
		-o $(@D)/cam-dump $(@D)/cam-dump.c
endef

define CAM_DUMP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cam-dump \
		$(TARGET_DIR)/usr/bin/cam-dump
endef

$(eval $(generic-package))
