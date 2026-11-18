################################################################################
# Tab5 BLE keyboard client: mgmt pair/bond + HOGP -> UHID (#41)
################################################################################

BLE_KBD_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/ble-kbd/src
BLE_KBD_SITE_METHOD = local
BLE_KBD_LICENSE = GPL-2.0-or-later

define BLE_KBD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-o $(@D)/ble-kbd $(@D)/ble-kbd.c
endef

define BLE_KBD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ble-kbd \
		$(TARGET_DIR)/usr/bin/ble-kbd
endef

$(eval $(generic-package))
