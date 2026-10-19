################################################################################
#
# micropython-nommu
#
################################################################################

MICROPYTHON_NOMMU_VERSION = 1.22.2
MICROPYTHON_NOMMU_SITE = https://micropython.org/resources/source
MICROPYTHON_NOMMU_SOURCE = micropython-$(MICROPYTHON_NOMMU_VERSION).tar.xz
MICROPYTHON_NOMMU_LICENSE = MIT
MICROPYTHON_NOMMU_LICENSE_FILES = LICENSE
MICROPYTHON_NOMMU_DEPENDENCIES = host-python3

# variant/: the unix "standard" modules within the NOMMU size budget (#34).
define MICROPYTHON_NOMMU_INSTALL_VARIANT
	mkdir -p $(@D)/ports/unix/variants/nommu
	cp $(MICROPYTHON_NOMMU_PKGDIR)/variant/* $(@D)/ports/unix/variants/nommu/
	# The unix port assumes a large native stack and checks for overflow at
	# 40000 bytes, above the bFLT stack below: deep recursion crashed with
	# SIGSEGV instead of raising RuntimeError.  Use the variant's limit.
	grep -q 'mp_uint_t stack_limit = 40000 \* (sizeof(void \*) / 4);' $(@D)/ports/unix/main.c
	sed -i 's|mp_uint_t stack_limit = 40000 \* (sizeof(void \*) / 4);|mp_uint_t stack_limit = MICROPY_NOMMU_STACK_LIMIT;|' \
		$(@D)/ports/unix/main.c
endef
MICROPYTHON_NOMMU_POST_PATCH_HOOKS += MICROPYTHON_NOMMU_INSTALL_VARIANT

define MICROPYTHON_NOMMU_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/mpy-cross
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/ports/unix \
		VARIANT=nommu \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		CFLAGS_EXTRA="-fPIC -DMICROPY_GCREGS_SETJMP=1 -DMICROPY_NLR_SETJMP=1 -Wno-error" \
		LDFLAGS_EXTRA="-fPIC -Wl,-elf2flt=-r -Wl,-elf2flt=-s49152-Wl,--allow-multiple-definition" \
		STRIP=true \
		SIZE=true
endef

define MICROPYTHON_NOMMU_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ports/unix/build-nommu/micropython \
		$(TARGET_DIR)/usr/bin/micropython
	ln -sf micropython $(TARGET_DIR)/usr/bin/python3
	ln -sf micropython $(TARGET_DIR)/usr/bin/python
endef

$(eval $(generic-package))
