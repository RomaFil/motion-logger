################################################################################
#
# motion_logger
#
################################################################################

MOTION_LOGGER_VERSION = 0.1
MOTION_LOGGER_SITE = $(BR2_EXTERNAL_MOTION_LOGGER_PATH)/app
MOTION_LOGGER_SITE_METHOD = local
MOTION_LOGGER_LICENSE = Proprietary

# BUILD винесено в каталог пакета: інакше host-збірка app/build потрапила б у rsync
# і Make міг би злинкувати x86-об'єкти в aarch64-програму.
define MOTION_LOGGER_BUILD_CMDS
	$(MAKE) -C $(@D) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" BUILD=$(@D)/.br-build
endef

define MOTION_LOGGER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/.br-build/motion_logger $(TARGET_DIR)/usr/bin/motion_logger
endef

$(eval $(generic-package))
