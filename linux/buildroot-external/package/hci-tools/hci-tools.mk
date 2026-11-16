################################################################################
# Tab5 raw-HCI tools: LE scanner + advertiser (#34)
################################################################################

HCI_TOOLS_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/hci-tools/src
HCI_TOOLS_SITE_METHOD = local
HCI_TOOLS_LICENSE = GPL-2.0-or-later

define HCI_TOOLS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/hci-scan $(@D)/hci-scan.c
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/hci-adv $(@D)/hci-adv.c
endef

define HCI_TOOLS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/hci-scan \
		$(TARGET_DIR)/usr/bin/hci-scan
	$(INSTALL) -D -m 0755 $(@D)/hci-adv \
		$(TARGET_DIR)/usr/bin/hci-adv
endef

$(eval $(generic-package))
