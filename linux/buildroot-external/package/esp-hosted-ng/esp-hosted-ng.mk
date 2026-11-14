################################################################################
# ESP-Hosted-NG SDIO host module
################################################################################

ESP_HOSTED_NG_VERSION = 8626b42fd3f9eb5a1ccb5daea481f0d8d32b1685
ESP_HOSTED_NG_SITE = $(BR2_EXTERNAL_STAMP_P4_PATH)/../vendor/esp-hosted/esp_hosted_ng/host
ESP_HOSTED_NG_SITE_METHOD = local
ESP_HOSTED_NG_LICENSE = GPL-2.0-only (driver), GPL-2.0-only or Apache-2.0 (shared headers)
ESP_HOSTED_NG_LICENSE_FILES = LICENSE
ESP_HOSTED_NG_POST_RSYNC_HOOKS += ESP_HOSTED_NG_NORMALIZE_LINE_ENDINGS ESP_HOSTED_NG_APPLY_PATCH
ESP_HOSTED_NG_MODULE_MAKE_OPTS = \
	target=sdio \
	ARCH=$(KERNEL_ARCH) \
	CROSS_COMPILE="$(TARGET_CROSS)" \
	CONFIG_BT_ENABLED=n \
	CONFIG_DEBUG_LOGS=n \
	CONFIG_AP_SUPPORT=y

define ESP_HOSTED_NG_NORMALIZE_LINE_ENDINGS
	find $(@D) -type f \( -name '*.c' -o -name '*.h' -o -name Makefile \) \
		-exec sed -i 's/\r$$//' {} +
endef

define ESP_HOSTED_NG_APPLY_PATCH
	$(APPLY_PATCHES) $(@D) $(BR2_EXTERNAL_STAMP_P4_PATH)/package/esp-hosted-ng \*.patch
endef

# Linux 6.15 removed EXTRA_CFLAGS from kbuild (e966ad0edd00), so the vendored
# Makefile's "EXTRA_CFLAGS += -DCONFIG_AP_MODE" (selected by CONFIG_AP_SUPPORT=y
# above) is silently ignored: esp_add_wiphy() then registers a station-only
# wiphy, wpa_supplicant refuses "mode=2" ("Driver does not support AP mode")
# and the SoftAP never transmits.  Move that one define to ccflags-y, and fail
# the build if the rewrite did not take effect.  KCFLAGS is not usable here:
# LINUX_MAKE_FLAGS already sets it (-Wno-attribute-alias, -mno-fdpic, ...).
define ESP_HOSTED_NG_AP_MODE_CCFLAGS
	sed -i 's/EXTRA_CFLAGS += -DCONFIG_AP_MODE/ccflags-y += -DCONFIG_AP_MODE/' $(@D)/Makefile
	grep -q 'ccflags-y += -DCONFIG_AP_MODE' $(@D)/Makefile
endef
ESP_HOSTED_NG_POST_RSYNC_HOOKS += ESP_HOSTED_NG_AP_MODE_CCFLAGS

define ESP_HOSTED_NG_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(LINUX_DIR) \
		$(LINUX_MAKE_FLAGS) M=$(@D) \
		$(ESP_HOSTED_NG_MODULE_MAKE_OPTS) modules
endef

define ESP_HOSTED_NG_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(LINUX_DIR) \
		$(LINUX_MAKE_FLAGS) M=$(@D) \
		$(ESP_HOSTED_NG_MODULE_MAKE_OPTS) \
		INSTALL_MOD_PATH=$(TARGET_DIR) modules_install
endef

$(eval $(generic-package))
