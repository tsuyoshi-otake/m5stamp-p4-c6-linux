################################################################################
#
# busybox-udhcpc
#
################################################################################

# Same source and Buildroot patches as the main BusyBox; only the config
# (busybox-udhcpc.config, applied over allnoconfig) differs.
BUSYBOX_UDHCPC_VERSION = $(BUSYBOX_VERSION)
BUSYBOX_UDHCPC_SITE = $(BUSYBOX_SITE)
BUSYBOX_UDHCPC_SOURCE = $(BUSYBOX_SOURCE)
BUSYBOX_UDHCPC_DL_SUBDIR = busybox
BUSYBOX_UDHCPC_LICENSE = GPL-2.0
BUSYBOX_UDHCPC_LICENSE_FILES = LICENSE
BUSYBOX_UDHCPC_DEPENDENCIES = busybox

define BUSYBOX_UDHCPC_APPLY_BUSYBOX_PATCHES
	$(APPLY_PATCHES) $(@D) $(BUSYBOX_PKGDIR) \*.patch
endef
BUSYBOX_UDHCPC_POST_PATCH_HOOKS += BUSYBOX_UDHCPC_APPLY_BUSYBOX_PATCHES

# allnoconfig in this kconfig does not keep the values from allno.config,
# so drop the listed symbols from its output, append the list and let
# oldconfig settle the dependencies.
define BUSYBOX_UDHCPC_CONFIGURE_CMDS
	$(BUSYBOX_MAKE_ENV) $(MAKE) $(BUSYBOX_MAKE_OPTS) -C $(@D) allnoconfig
	sed -n 's/^\(# \)\{0,1\}\(CONFIG_[A-Z0-9_]*\)[= ].*/\2/p' \
		$(BUSYBOX_UDHCPC_PKGDIR)/busybox-udhcpc.config > $(@D)/.udhcpc-keys
	grep -v -w -F -f $(@D)/.udhcpc-keys $(@D)/.config > $(@D)/.config.base
	cat $(@D)/.config.base $(BUSYBOX_UDHCPC_PKGDIR)/busybox-udhcpc.config \
		> $(@D)/.config
	yes "" | $(BUSYBOX_MAKE_ENV) $(MAKE) $(BUSYBOX_MAKE_OPTS) -C $(@D) oldconfig
	grep '^\(# \)\{0,1\}CONFIG_' $(BUSYBOX_UDHCPC_PKGDIR)/busybox-udhcpc.config | \
		while IFS= read -r line; do \
			grep -qxF -- "$$line" $(@D)/.config || { \
				echo "busybox-udhcpc: oldconfig dropped: $$line" >&2; exit 1; }; \
		done
endef

define BUSYBOX_UDHCPC_BUILD_CMDS
	$(BUSYBOX_MAKE_ENV) $(MAKE) $(BUSYBOX_MAKE_OPTS) -C $(@D) busybox
endef

define BUSYBOX_UDHCPC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/busybox $(TARGET_DIR)/bin/busybox-udhcpc
endef

$(eval $(generic-package))
