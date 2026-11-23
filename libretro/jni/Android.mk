LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE       := retro
LOCAL_SRC_FILES    := ../libretro.cpp
LOCAL_CPPFLAGS     := -O3 -std=gnu++17 -fexceptions -frtti -Wno-unused-function
LOCAL_LDFLAGS      := -Wl,--version-script=$(LOCAL_PATH)/../link.T
include $(BUILD_SHARED_LIBRARY)
