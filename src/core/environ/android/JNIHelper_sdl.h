#pragma once
#include <jni.h>
#include <string>
#include <vector>
#include <functional>
#include <SDL3/SDL_system.h>

namespace jni {

extern JavaVM *g_JVM;

struct JniMethodInfo {
	JNIEnv *env = nullptr;
	jclass classID = nullptr;
	jmethodID methodID = nullptr;
};

inline JNIEnv *GetEnv() {
	// SDL3 provides thread-safe JNIEnv access on SDL-managed threads
	JNIEnv *env = (JNIEnv*)SDL_GetAndroidJNIEnv();
	if (env) return env;
	// Fallback for threads not managed by SDL
	if (!g_JVM) return nullptr;
	jint ret = g_JVM->GetEnv((void**)&env, JNI_VERSION_1_4);
	if (ret == JNI_EDETACHED)
		g_JVM->AttachCurrentThread(&env, nullptr);
	return env;
}

inline bool getStaticMethodInfo(JniMethodInfo &info, const char *cls,
		const char *name, const char *sig) {
	info.env = GetEnv();
	if (!info.env) return false;
	info.classID = info.env->FindClass(cls);
	if (!info.classID) return false;
	info.methodID = info.env->GetStaticMethodID(info.classID, name, sig);
	return info.methodID != nullptr;
}

inline bool getMethodInfo(JniMethodInfo &info, const char *cls,
		const char *name, const char *sig) {
	info.env = GetEnv();
	if (!info.env) return false;
	info.classID = info.env->FindClass(cls);
	if (!info.classID) return false;
	info.methodID = info.env->GetMethodID(info.classID, name, sig);
	return info.methodID != nullptr;
}

inline std::string jstring2string(JNIEnv *env, jstring jstr) {
	if (!jstr) return {};
	const char *utf = env->GetStringUTFChars(jstr, nullptr);
	if (!utf) return {};
	std::string ret(utf);
	env->ReleaseStringUTFChars(jstr, utf);
	env->DeleteLocalRef(jstr);
	return ret;
}

} // namespace jni

// Startup path/args stashed by nativeSetStartupArgs, consumed by TVPCheckStartupArg
extern bool TVPCheckStartupArg();
extern std::string g_AndroidStartupPath;
extern std::vector<std::string> g_AndroidStartupArgs;

// Event queue for JNI thread-to-main-thread dispatch
void Android_PushEvents(std::function<void()> func);
void DrainAndroidEventQueue();
