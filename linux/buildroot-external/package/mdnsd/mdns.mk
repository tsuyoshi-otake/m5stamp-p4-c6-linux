################################################################################
# Stamp-P4 minimal mDNS responder
################################################################################

MDNSD_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/mdnsd/src
MDNSD_SITE_METHOD = local
MDNSD_LICENSE = GPL-2.0-only

define MDNSD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/mdnsd \
		$(@D)/mdnsd.c
endef

define MDNSD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mdnsd \
		$(TARGET_DIR)/usr/sbin/mdnsd
endef

$(eval $(generic-package))
