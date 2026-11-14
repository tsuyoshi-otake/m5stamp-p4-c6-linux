P4_DEMO_VERSION = 1.0
P4_DEMO_SITE = $(BR2_EXTERNAL_STAMP_P4_PATH)/package/p4-demo/src
P4_DEMO_SITE_METHOD = local
P4_DEMO_LICENSE = GPL-2.0-only

define P4_DEMO_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra -Werror \
		$(TARGET_LDFLAGS) -o $(@D)/p4-demo $(@D)/p4-demo.c
endef

define P4_DEMO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/p4-demo $(TARGET_DIR)/usr/bin/p4-demo
endef

$(eval $(generic-package))
