################################################################################
# Tab5 Wi-Fi credentials QR helper
################################################################################

WIFI_QR_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/wifi-qr/src
WIFI_QR_SITE_METHOD = local
WIFI_QR_LICENSE = GPL-2.0-only, MIT
# MIT text is the header comment of src/qrcodegen.[ch] (Project Nayuki).

define WIFI_QR_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/wifi-qr \
		$(@D)/wifi-qr.c $(@D)/wqr_payload.c $(@D)/qrcodegen.c
endef

define WIFI_QR_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/wifi-qr \
		$(TARGET_DIR)/usr/bin/wifi-qr
endef

$(eval $(generic-package))
