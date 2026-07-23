#pragma once
#include <cstdint>
using jint = int;
using jlong = long long;
using jboolean = unsigned char;
using jobject = void *;
using jstring = void *;
using jintArray = void *;
using jobjectArray = void *;
constexpr jint JNI_VERSION_1_6 = 0x00010006;
constexpr jint JNI_OK = 0;
constexpr jint JNI_EDETACHED = -2;
struct JNINativeMethod {
  const char *name;
  const char *signature;
  void *fnPtr;
};
struct JavaVM;
struct JNIEnv {
  jint GetJavaVM(JavaVM **) { return JNI_OK; }
  const char *GetStringUTFChars(jstring, jboolean *) { return nullptr; }
  void ReleaseStringUTFChars(jstring, const char *) {}
};
struct JavaVM {
  jint GetEnv(void **, jint) { return JNI_OK; }
  jint AttachCurrentThread(JNIEnv **, void *) { return JNI_OK; }
  jint DetachCurrentThread() { return JNI_OK; }
};
