################################################################################
# EasyStick minimal mDNS responder
################################################################################

EASYSTICK_MDNS_SITE = \
	$(BR2_EXTERNAL_EASYSTICK_STAMP_P4_PATH)/package/easystick-mdns/src
EASYSTICK_MDNS_SITE_METHOD = local
EASYSTICK_MDNS_LICENSE = GPL-2.0-only

define EASYSTICK_MDNS_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/easystick-mdns \
		$(@D)/easystick-mdns.c
endef

define EASYSTICK_MDNS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/easystick-mdns \
		$(TARGET_DIR)/usr/sbin/easystick-mdns
endef

$(eval $(generic-package))
