################################################################################
#
# micropython-nommu
#
################################################################################

MICROPYTHON_NOMMU_VERSION = 1.22.2
MICROPYTHON_NOMMU_SITE = https://micropython.org/resources/source
MICROPYTHON_NOMMU_SOURCE = micropython-$(MICROPYTHON_NOMMU_VERSION).tar.xz
MICROPYTHON_NOMMU_LICENSE = MIT
MICROPYTHON_NOMMU_DEPENDENCIES = host-python3

define MICROPYTHON_NOMMU_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/mpy-cross
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/ports/unix \
		VARIANT=minimal \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		CFLAGS_EXTRA="-fPIC -DMICROPY_GCREGS_SETJMP=1 -DMICROPY_NLR_SETJMP=1 -Wno-error" \
		LDFLAGS_EXTRA="-fPIC -Wl,-elf2flt=-r -Wl,-elf2flt=-s32768 -Wl,--allow-multiple-definition" \
		STRIP=true \
		SIZE=true
endef

define MICROPYTHON_NOMMU_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ports/unix/build-minimal/micropython \
		$(TARGET_DIR)/usr/bin/micropython
	ln -sf micropython $(TARGET_DIR)/usr/bin/python3
	ln -sf micropython $(TARGET_DIR)/usr/bin/python
endef

$(eval $(generic-package))
