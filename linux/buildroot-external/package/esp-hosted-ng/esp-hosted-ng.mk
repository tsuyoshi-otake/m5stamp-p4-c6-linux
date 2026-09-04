################################################################################
# ESP-Hosted-NG SDIO host module
################################################################################

ESP_HOSTED_NG_VERSION = 8626b42fd3f9eb5a1ccb5daea481f0d8d32b1685
ESP_HOSTED_NG_SITE = $(BR2_EXTERNAL_EASYSTICK_STAMP_P4_PATH)/../vendor/esp-hosted/esp_hosted_ng/host
ESP_HOSTED_NG_SITE_METHOD = local
ESP_HOSTED_NG_LICENSE = Apache-2.0
ESP_HOSTED_NG_LICENSE_FILES = LICENSE
ESP_HOSTED_NG_POST_RSYNC_HOOKS += ESP_HOSTED_NG_APPLY_PATCH
ESP_HOSTED_NG_MODULE_MAKE_OPTS = \
	target=sdio \
	ARCH=$(KERNEL_ARCH) \
	CROSS_COMPILE="$(TARGET_CROSS)" \
	CONFIG_BT_ENABLED=n \
	CONFIG_DEBUG_LOGS=n

define ESP_HOSTED_NG_APPLY_PATCH
	$(APPLY_PATCHES) $(@D) $(BR2_EXTERNAL_EASYSTICK_STAMP_P4_PATH)/package/esp-hosted-ng \*.patch
endef

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
