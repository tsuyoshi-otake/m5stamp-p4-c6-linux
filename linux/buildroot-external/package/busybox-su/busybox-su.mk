################################################################################
#
# busybox-su
#
################################################################################

# Same source and Buildroot patches as the main BusyBox; only the config
# (busybox-su.config, applied over allnoconfig) differs.
BUSYBOX_SU_VERSION = $(BUSYBOX_VERSION)
BUSYBOX_SU_SITE = $(BUSYBOX_SITE)
BUSYBOX_SU_SOURCE = $(BUSYBOX_SOURCE)
BUSYBOX_SU_DL_SUBDIR = busybox
BUSYBOX_SU_LICENSE = GPL-2.0
BUSYBOX_SU_LICENSE_FILES = LICENSE
BUSYBOX_SU_DEPENDENCIES = busybox

define BUSYBOX_SU_APPLY_BUSYBOX_PATCHES
	$(APPLY_PATCHES) $(@D) $(BUSYBOX_PKGDIR) \*.patch
endef
BUSYBOX_SU_POST_PATCH_HOOKS += BUSYBOX_SU_APPLY_BUSYBOX_PATCHES

# allnoconfig in this kconfig does not keep the values from allno.config,
# so drop the listed symbols from its output, append the list and let
# oldconfig settle the dependencies.
define BUSYBOX_SU_CONFIGURE_CMDS
	$(BUSYBOX_MAKE_ENV) $(MAKE) $(BUSYBOX_MAKE_OPTS) -C $(@D) allnoconfig
	sed -n 's/^\(# \)\{0,1\}\(CONFIG_[A-Z0-9_]*\)[= ].*/\2/p' \
		$(BUSYBOX_SU_PKGDIR)/busybox-su.config > $(@D)/.su-keys
	grep -v -w -F -f $(@D)/.su-keys $(@D)/.config > $(@D)/.config.base
	cat $(@D)/.config.base $(BUSYBOX_SU_PKGDIR)/busybox-su.config \
		> $(@D)/.config
	yes "" | $(BUSYBOX_MAKE_ENV) $(MAKE) $(BUSYBOX_MAKE_OPTS) -C $(@D) oldconfig
	grep '^\(# \)\{0,1\}CONFIG_' $(BUSYBOX_SU_PKGDIR)/busybox-su.config | \
		while IFS= read -r line; do \
			grep -qxF -- "$$line" $(@D)/.config || { \
				echo "busybox-su: oldconfig dropped: $$line" >&2; exit 1; }; \
		done
endef

define BUSYBOX_SU_BUILD_CMDS
	$(BUSYBOX_MAKE_ENV) $(MAKE) $(BUSYBOX_MAKE_OPTS) -C $(@D) busybox
endef

# The only setuid file in the image: m5 -> root over SSH (#37).  The plain
# install lands 0755; the permissions table applies the setuid bit so the
# squashfs keeps it.
define BUSYBOX_SU_PERMISSIONS
	/bin/busybox-su f 4755 0 0 - - - - -
endef

define BUSYBOX_SU_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/busybox $(TARGET_DIR)/bin/busybox-su
	ln -sfn busybox-su $(TARGET_DIR)/bin/su
endef

$(eval $(generic-package))
