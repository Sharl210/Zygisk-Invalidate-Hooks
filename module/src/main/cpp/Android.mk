LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := xdl
LOCAL_SRC_FILES := xdl/libs/$(TARGET_ARCH_ABI)/libxdl.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/xdl
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := inline_hook_spoof
LOCAL_SRC_FILES := main.cpp mainCore.cxx
LOCAL_C_INCLUDES := $(LOCAL_PATH)/xdl
LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra -Werror=return-type
LOCAL_STATIC_LIBRARIES := xdl
LOCAL_LDLIBS := -llog -ldl
include $(BUILD_SHARED_LIBRARY)
