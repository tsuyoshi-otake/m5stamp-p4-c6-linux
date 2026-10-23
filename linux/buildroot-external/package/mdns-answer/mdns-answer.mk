################################################################################
# Stamp-P4 minimal mDNS responder
################################################################################

MDNS_ANSWER_SITE = \
	$(BR2_EXTERNAL_STAMP_P4_PATH)/package/mdns-answer/src
MDNS_ANSWER_SITE_METHOD = local
MDNS_ANSWER_LICENSE = GPL-2.0-only

define MDNS_ANSWER_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/mdnsd \
		$(@D)/mdnsd.c
endef

define MDNS_ANSWER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mdnsd \
		$(TARGET_DIR)/usr/sbin/mdnsd
endef

MDNS_ANSWER_VERSION = 1.0
$(eval $(generic-package))
