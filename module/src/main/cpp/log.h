#ifndef _LOG_H_
#define _LOG_H_

#include <android/log.h>

#define TAG "ANTIK"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#endif // _LOG_H_