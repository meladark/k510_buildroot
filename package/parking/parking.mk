################################################################################
#
# parking
#
################################################################################
PARKING_LOCAL_PATH := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
PARKING_SITE = $(PARKING_LOCAL_PATH)/code
PARKING_SITE_METHOD = local

PARKING_DEPENDENCIES += ai mediactl_lib nncase_linux_runtime opencv4 libdrm rapidjson freetype venc_lib ffmpeg_canaan

PARKING_CONF_OPTS += \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_CXX_FLAGS="$(TARGET_CXXFLAGS) -I$(STAGING_DIR)/usr/include/opencv4 -I$(STAGING_DIR)/usr/include/libdrm -I$(STAGING_DIR)/usr/include/freetype2 -I$(STAGING_DIR)/usr/local/include" \
	-DCMAKE_EXE_LINKER_FLAGS="-L$(STAGING_DIR)/usr/local/lib -L$(STAGING_DIR)/lib64/lp64d" \
	-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) -I$(STAGING_DIR)/usr/include/libdrm -I$(STAGING_DIR)/usr/include" \
	-DCMAKE_INSTALL_PREFIX="/app/parking"

define PARKING_INSTALL_INIT_SCRIPT
	$(INSTALL) -D -m 0755 $(PARKING_LOCAL_PATH)/S99launcher $(TARGET_DIR)/etc/init.d/launcher
	rm -f $(TARGET_DIR)/etc/init.d/parking $(TARGET_DIR)/app/parking/parking.sh
endef
PARKING_POST_INSTALL_TARGET_HOOKS += PARKING_INSTALL_INIT_SCRIPT

$(eval $(cmake-package))
