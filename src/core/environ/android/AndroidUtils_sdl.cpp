//---------------------------------------------------------------------------
// Android platform utilities for SDL2 build variant
// (replaces AndroidUtils.cpp — no cocos2d dependency)
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <mutex>
#include <queue>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <functional>

#include "Application.h"
#include "Platform.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "EventIntf.h"
#include "SysInitIntf.h"
#include "StorageIntf.h"
#include "ScriptMgnIntf.h"
#include "WindowIntf.h"
#include "ConfigManager/GlobalConfigManager.h"
#include "ConfigManager/LocaleConfigManager.h"
#include "JNIHelper_sdl.h"
#include "StorageImpl.h"

#define TAG "##krkr"
#define KR2ACT_PATH "org/tvp/kirikiri2/KR2Activity"

using namespace jni;

//---------------------------------------------------------------------------
// TVPGetRoughTickCount32
//---------------------------------------------------------------------------
tjs_uint TVPGetRoughTickCount32() {
	static auto start = std::chrono::steady_clock::now();
	auto now = std::chrono::steady_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
	return (tjs_uint)ms;
}

//---------------------------------------------------------------------------
// TVP_stat
//---------------------------------------------------------------------------
#include <sys/stat.h>

bool TVP_stat(const tjs_char *name, tTVP_stat &s) {
	std::string n;
	int len = (int)TJS_wcstombs(nullptr, name, 0);
	if (len < 0) return false;
	char *tmp = new char[len + 1];
	TJS_wcstombs(tmp, name, len);
	tmp[len] = 0;
	n = tmp;
	delete[] tmp;
	return TVP_stat(n.c_str(), s);
}

bool TVP_stat(const char *name, tTVP_stat &s) {
	struct stat t;
	bool ret = !stat(name, &t);
	s.st_mode = t.st_mode;
	s.st_size = t.st_size;
#pragma push_macro("st_atime")
#undef st_atime
	s.st_atime_sec = (uint64_t)t.st_atim.tv_sec;
#pragma pop_macro("st_atime")
	s.st_mtime_sec = (uint64_t)t.st_mtim.tv_sec;
	s.st_ctime_sec = (uint64_t)t.st_ctim.tv_sec;
	return ret;
}

//---------------------------------------------------------------------------
// TVP_utime
//---------------------------------------------------------------------------
void TVP_utime(const char *name, time_t modtime) {
	struct timeval times[2] = {
		{ modtime, 0 },
		{ modtime, 0 }
	};
	utimes(name, times);
}

//---------------------------------------------------------------------------
// TVPForceSwapBuffer — defined in WindowLayer_sdl.cpp
//---------------------------------------------------------------------------
extern void TVPForceSwapBuffer();

//---------------------------------------------------------------------------
// Memory info
//---------------------------------------------------------------------------
static void _updateMemoryInfo() {
	static tjs_uint32 _lastQuery = 0;
	tjs_uint32 now = TVPGetRoughTickCount32();
	if (_lastQuery > 0 && now - _lastQuery < 3000) return;
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "updateMemoryInfo", "()V")) {
		t.env->CallStaticVoidMethod(t.classID, t.methodID);
		t.env->DeleteLocalRef(t.classID);
		_lastQuery = now;
	}
}

tjs_int TVPGetSystemFreeMemory() {
	_updateMemoryInfo();
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "getAvailMemory", "()J")) {
		jlong ret = t.env->CallStaticLongMethod(t.classID, t.methodID);
		t.env->DeleteLocalRef(t.classID);
		return (tjs_int)(ret / (1024 * 1024));
	}
	return 0;
}

tjs_int TVPGetSelfUsedMemory() {
	_updateMemoryInfo();
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "getUsedMemory", "()J")) {
		jlong ret = t.env->CallStaticLongMethod(t.classID, t.methodID);
		t.env->DeleteLocalRef(t.classID);
		return (tjs_int)(ret >> 10);
	}
	return 0;
}

//---------------------------------------------------------------------------
// TVPGetPackageVersionString
//---------------------------------------------------------------------------
std::string TVPGetPackageVersionString() {
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "GetInstance", "()Ljava/lang/Object;")) {
		jobject inst = t.env->CallStaticObjectMethod(t.classID, t.methodID);
		t.env->DeleteLocalRef(t.classID);
		if (!inst) return "";
		JniMethodInfo t2;
		if (getMethodInfo(t2, KR2ACT_PATH, "getPackageVersionString", "()Ljava/lang/String;")) {
			jstring str = (jstring)t2.env->CallObjectMethod(inst, t2.methodID);
			t2.env->DeleteLocalRef(t2.classID);
			return jstring2string(t2.env, str);
		}
		t.env->DeleteLocalRef(inst);
	}
	return "";
}

//---------------------------------------------------------------------------
// TVPGetAppStoragePath / TVPGetDriverPath
//---------------------------------------------------------------------------
static std::vector<std::string> _getStoragePathList(const char *method) {
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, method, "()Ljava/lang/String;")) {
		jstring ret = (jstring)t.env->CallStaticObjectMethod(t.classID, t.methodID);
		t.env->DeleteLocalRef(t.classID);
		return { jstring2string(t.env, ret) };
	}
	return {};
}

std::vector<std::string> TVPGetAppStoragePath() {
	auto p = _getStoragePathList("getExternalStoragePath");
	if (p.empty() || p[0].empty()) p = _getStoragePathList("getInternalStoragePath");
	return p;
}

std::vector<std::string> TVPGetDriverPath() {
	return _getStoragePathList("getDriverPath");
}

//---------------------------------------------------------------------------
// TVPShowSimpleMessageBox
//---------------------------------------------------------------------------
namespace kr2android {
	extern std::condition_variable MessageBoxCond;
	extern std::mutex MessageBoxLock;
	extern int MsgBoxRet;
	extern std::string MessageBoxRetText;
}

extern "C" int TVPShowSimpleMessageBox(const char *text, const char *title,
		unsigned nBtn, const char **btnText) {
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "ShowMessageBox",
			"(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)V")) {
		jstring jt = t.env->NewStringUTF(title);
		jstring jx = t.env->NewStringUTF(text);
		jclass scls = t.env->FindClass("java/lang/String");
		jobjectArray btns = t.env->NewObjectArray(nBtn, scls, nullptr);
		for (unsigned i = 0; i < nBtn; ++i) {
			jstring jb = t.env->NewStringUTF(btnText[i]);
			t.env->SetObjectArrayElement(btns, i, jb);
			t.env->DeleteLocalRef(jb);
		}
		t.env->CallStaticVoidMethod(t.classID, t.methodID, jt, jx, btns);
		t.env->DeleteLocalRef(jt);
		t.env->DeleteLocalRef(jx);
		t.env->DeleteLocalRef(btns);
		t.env->DeleteLocalRef(scls);
		t.env->DeleteLocalRef(t.classID);

		kr2android::MsgBoxRet = -2;
		{
			std::unique_lock<std::mutex> lk(kr2android::MessageBoxLock);
			while (kr2android::MsgBoxRet == -2) {
				kr2android::MessageBoxCond.wait_for(lk,
					std::chrono::milliseconds(16));
				if (kr2android::MsgBoxRet != -2) break;
				lk.unlock();
				TVPForceSwapBuffer();
				lk.lock();
			}
		}
		return kr2android::MsgBoxRet;
	}
	return -1;
}

int TVPShowSimpleMessageBox(const ttstr &text, const ttstr &caption,
		const std::vector<ttstr> &btns) {
	std::vector<const char *> ptrs;
	std::vector<std::string> holders;
	holders.reserve(btns.size());
	for (auto &b : btns) {
		holders.push_back(b.AsNarrowStdString());
		ptrs.push_back(holders.back().c_str());
	}
	return TVPShowSimpleMessageBox(text.AsNarrowStdString().c_str(),
			caption.AsNarrowStdString().c_str(),
			(unsigned)ptrs.size(), ptrs.data());
}

int TVPShowSimpleMessageBox(const ttstr &text, const ttstr &caption) {
	std::vector<ttstr> btns = { ttstr(TJS_W("Ok")) };
	return TVPShowSimpleMessageBox(text, caption, btns);
}

int TVPShowSimpleMessageBoxYesNo(const ttstr &text, const ttstr &caption) {
	std::vector<ttstr> btns = { ttstr(TJS_W("Yes")), ttstr(TJS_W("No")) };
	return TVPShowSimpleMessageBox(text, caption, btns);
}

//---------------------------------------------------------------------------
// TVPShowSimpleInputBox
//---------------------------------------------------------------------------
int TVPShowSimpleInputBox(ttstr &text, const ttstr &caption,
		const ttstr &prompt, const std::vector<ttstr> &btns) {
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "ShowInputBox",
			"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)V")) {
		jstring jtxt = t.env->NewStringUTF(text.AsNarrowStdString().c_str());
		jstring jcap = t.env->NewStringUTF(caption.AsNarrowStdString().c_str());
		jstring jprm = t.env->NewStringUTF(prompt.AsNarrowStdString().c_str());
		jclass scls = t.env->FindClass("java/lang/String");
		jobjectArray jbtns = t.env->NewObjectArray(btns.size(), scls, nullptr);
		for (size_t i = 0; i < btns.size(); ++i) {
			jstring jb = t.env->NewStringUTF(btns[i].AsNarrowStdString().c_str());
			t.env->SetObjectArrayElement(jbtns, i, jb);
			t.env->DeleteLocalRef(jb);
		}
		t.env->CallStaticVoidMethod(t.classID, t.methodID, jcap, jtxt, jprm, jbtns);
		t.env->DeleteLocalRef(jtxt);
		t.env->DeleteLocalRef(jcap);
		t.env->DeleteLocalRef(jprm);
		t.env->DeleteLocalRef(jbtns);
		t.env->DeleteLocalRef(scls);
		t.env->DeleteLocalRef(t.classID);

		kr2android::MsgBoxRet = -2;
		{
			std::unique_lock<std::mutex> lk(kr2android::MessageBoxLock);
			while (kr2android::MsgBoxRet == -2) {
				kr2android::MessageBoxLock.unlock();
				TVPForceSwapBuffer();
				{
					std::unique_lock<std::mutex> lk2(kr2android::MessageBoxLock);
					(void)lk2;
				}
				kr2android::MessageBoxLock.lock();
			}
		}
		return kr2android::MsgBoxRet;
	}
	return -1;
}

//---------------------------------------------------------------------------
// TVPExitApplication
//---------------------------------------------------------------------------
void TVPExitApplication(int code) {
	__android_log_print(ANDROID_LOG_INFO, "##krkr", "TVPExitApplication code=%d", code);
	TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_MAX);
	// Set termination flag so the main loop exits cleanly
	::Application->Terminate();
	// Call Java exit() which posts finish() to the UI thread
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "exit", "()V")) {
		t.env->CallStaticVoidMethod(t.classID, t.methodID);
		t.env->DeleteLocalRef(t.classID);
	}
}

//---------------------------------------------------------------------------
// TVPHideIME / TVPShowIME
//---------------------------------------------------------------------------
void TVPHideIME() {
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "hideTextInput", "()V")) {
		t.env->CallStaticVoidMethod(t.classID, t.methodID);
		t.env->DeleteLocalRef(t.classID);
	}
}

void TVPShowIME(int x, int y, int w, int h) {
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "showTextInput", "(IIII)V")) {
		t.env->CallStaticVoidMethod(t.classID, t.methodID, x, y, w, h);
		t.env->DeleteLocalRef(t.classID);
	}
}

//---------------------------------------------------------------------------
// TVPGetCurrentLanguage
//---------------------------------------------------------------------------
std::string TVPGetCurrentLanguage() {
	// Get default locale directly without caching class references
	JNIEnv *env = GetEnv();
	if (!env) return "zh";

	// Use raw JNI to avoid NewGlobalRef lifecycle issues
	jclass localeClass = env->FindClass("java/util/Locale");
	if (!localeClass) return "zh";

	jmethodID getDefaultId = env->GetStaticMethodID(localeClass, "getDefault", "()Ljava/util/Locale;");
	jobject locale = env->CallStaticObjectMethod(localeClass, getDefaultId);
	if (!locale) { env->DeleteLocalRef(localeClass); return "zh"; }

	jmethodID getLangId = env->GetMethodID(localeClass, "getLanguage", "()Ljava/lang/String;");
	jstring lang = (jstring)env->CallObjectMethod(locale, getLangId);

	std::string result;
	if (lang) {
		const char *utf = env->GetStringUTFChars(lang, nullptr);
		if (utf) { result = utf; env->ReleaseStringUTFChars(lang, utf); }
		env->DeleteLocalRef(lang);
	}

	env->DeleteLocalRef(locale);
	env->DeleteLocalRef(localeClass);
	return result.empty() ? "zh" : result;
}

//---------------------------------------------------------------------------
// TVPCreateFolders
//---------------------------------------------------------------------------
bool TVPCreateFolders(const ttstr &folder) {
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "createFolders",
			"(Ljava/lang/String;)Z")) {
		std::string f = folder.AsNarrowStdString();
		jstring jf = t.env->NewStringUTF(f.c_str());
		bool ret = t.env->CallStaticBooleanMethod(t.classID, t.methodID, jf);
		t.env->DeleteLocalRef(jf);
		t.env->DeleteLocalRef(t.classID);
		return ret;
	}
	return false;
}

//---------------------------------------------------------------------------
// TVPDeleteFile / TVPRenameFile / TVPCopyFile
//---------------------------------------------------------------------------
bool TVPDeleteFile(const std::string &file) {
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "deleteFile",
			"(Ljava/lang/String;)Z")) {
		jstring jf = t.env->NewStringUTF(file.c_str());
		bool ret = t.env->CallStaticBooleanMethod(t.classID, t.methodID, jf);
		t.env->DeleteLocalRef(jf);
		t.env->DeleteLocalRef(t.classID);
		return ret;
	}
	return false;
}

bool TVPRenameFile(const std::string &old, const std::string &new_) {
	JniMethodInfo t;
	if (getStaticMethodInfo(t, KR2ACT_PATH, "renameFile",
			"(Ljava/lang/String;Ljava/lang/String;)Z")) {
		jstring jo = t.env->NewStringUTF(old.c_str());
		jstring jn = t.env->NewStringUTF(new_.c_str());
		bool ret = t.env->CallStaticBooleanMethod(t.classID, t.methodID, jo, jn);
		t.env->DeleteLocalRef(jo);
		t.env->DeleteLocalRef(jn);
		t.env->DeleteLocalRef(t.classID);
		return ret;
	}
	return false;
}

bool TVPCopyFile(const std::string &from, const std::string &to) {
	FILE *f = fopen(from.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	FILE *t = fopen(to.c_str(), "wb");
	if (!t) { fclose(f); return false; }
	char buf[8192];
	while (sz > 0) {
		int n = fread(buf, 1, std::min((long)sizeof(buf), sz), f);
		if (n <= 0) break;
		fwrite(buf, 1, n, t);
		sz -= n;
	}
	fclose(f);
	fclose(t);
	return true;
}

//---------------------------------------------------------------------------
// TVPProcessInputEvents / TVPSendToOtherApp / TVPRelinquishCPU
//---------------------------------------------------------------------------
void TVPProcessInputEvents() {}
void TVPSendToOtherApp(const std::string &) {}
void TVPRelinquishCPU() {}

//---------------------------------------------------------------------------
// TVPGetMemoryInfo
//---------------------------------------------------------------------------
void TVPGetMemoryInfo(TVPMemoryInfo &m) {
	m.MemTotal = 0; m.MemFree = 0;
	m.SwapTotal = 0; m.SwapFree = 0;
	m.VirtualTotal = 0; m.VirtualUsed = 0;
}

//---------------------------------------------------------------------------
// TVPCheckMemory
//---------------------------------------------------------------------------
void TVPCheckMemory() {}

//---------------------------------------------------------------------------
// TVPPrintLog
//---------------------------------------------------------------------------
void TVPPrintLog(const char *str) {
	__android_log_print(ANDROID_LOG_INFO, TAG, "%s", str);
}

//---------------------------------------------------------------------------
// TVPFetchSDCardPermission
//---------------------------------------------------------------------------
void TVPFetchSDCardPermission() {}

//---------------------------------------------------------------------------
// TVPControlAdDialog
//---------------------------------------------------------------------------
void TVPControlAdDialog(int, int, int) {}

//---------------------------------------------------------------------------
// TVPGetDeviceID / TVPGetDeviceLanguage (stubs — currently unused)
//---------------------------------------------------------------------------
std::string TVPGetDeviceID() { return ""; }
std::string TVPGetDeviceLanguage() { return ""; }

//---------------------------------------------------------------------------
// TVPCheckStartupPath
//---------------------------------------------------------------------------
bool TVPCheckStartupPath(const std::string &path) {
	(void)path;
	return true; // FIXME: implement SAF permission check
}

//---------------------------------------------------------------------------
// TVPShowGamePicker — scan Download dir and auto-start if single game found
//---------------------------------------------------------------------------
#include <dirent.h>
#include <sys/stat.h>
void TVPShowGamePicker() {
	// Scan common paths for .xp3 files
	const char *roots[] = {
		"/storage/emulated/0/Download",
		"/storage/emulated/0/Android/data/com.yuri.kirikiri2/files",
	};
	std::vector<std::string> candidates;
	for (const char *root : roots) {
		DIR *d = opendir(root);
		if (!d) continue;
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			std::string full = std::string(root) + "/" + e->d_name;
			struct stat st;
			if (stat(full.c_str(), &st)) continue;
		if (S_ISREG(st.st_mode)) {
			// Check if it's an .xp3
			std::string n(e->d_name);
			if (n.size() > 4 && n.substr(n.size()-4) == ".xp3") {
				candidates.push_back(full);
			}
		} else if (S_ISDIR(st.st_mode)) {
			// Check for startup.tjs inside
			std::string sj = full + "/startup.tjs";
			if (stat(sj.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
				candidates.push_back(full);
			} else {
				// Also check for .xp3 files inside subdirectories
				DIR *sd = opendir(full.c_str());
				if (sd) {
					struct dirent *se;
					while ((se = readdir(sd))) {
						std::string sn(se->d_name);
						if (sn.size() > 4 && sn.substr(sn.size()-4) == ".xp3") {
							candidates.push_back(full + "/" + sn);
						}
					}
					closedir(sd);
				}
			}
		}
		}
		closedir(d);
	}

	if (candidates.empty()) {
		__android_log_print(ANDROID_LOG_INFO, TAG, "No games found");
		return;
	}
	if (candidates.size() == 1) {
		// Auto-start
		__android_log_print(ANDROID_LOG_INFO, TAG,
			"Auto-starting: %s", candidates[0].c_str());
		g_AndroidStartupPath = candidates[0];
		TVPCheckStartupArg();
		return;
	}
	// Multiple candidates: log them for now
	// FIXME: show JNI dialog to let user pick
	__android_log_print(ANDROID_LOG_INFO, TAG, "Multiple games found, use --es startupPath to select:");
	for (auto &c : candidates)
		__android_log_print(ANDROID_LOG_INFO, TAG, "  %s", c.c_str());
}

//---------------------------------------------------------------------------
// TVPWriteDataToFile
//---------------------------------------------------------------------------
bool TVPWriteDataToFile(const ttstr &file, const void *data, tjs_uint len) {
	// extract directory from path
	std::string f = file.AsNarrowStdString();
	auto pos = f.find_last_of("/\\");
	if (pos != std::string::npos) {
		TVPCreateFolders(ttstr(f.substr(0, pos)));
	}
	FILE *fp = fopen(f.c_str(), "wb");
	if (!fp) return false;
	fwrite(data, 1, len, fp);
	fclose(fp);
	return true;
}

//---------------------------------------------------------------------------
// TVPGetInternalPreferencePath
//---------------------------------------------------------------------------
const std::string &TVPGetInternalPreferencePath() {
	static std::string path = TVPGetAppStoragePath().empty()
		? "" : TVPGetAppStoragePath()[0] + "/.preference/";
	return path;
}

//---------------------------------------------------------------------------
// Stored startup path/args (set by JNI, read by TVPCheckStartupArg)
//---------------------------------------------------------------------------
std::string g_AndroidStartupPath;
std::vector<std::string> g_AndroidStartupArgs;

//---------------------------------------------------------------------------
// TVPCheckStartupArg
//---------------------------------------------------------------------------
bool TVPCheckStartupArg() {
	// consume launch arguments stashed by nativeSetStartupArgs
	const std::string &startupPath = g_AndroidStartupPath;
	if (startupPath.empty()) return false;

	// Check if the path points to a bootable archive (.xp3)
	ttstr tjsPath(startupPath);
	bool bootable = false;
	if (TVPCheckExistentLocalFile(tjsPath)) {
		// Single file — check if it's an XP3 archive
		tTJSBinaryStream *st = TVPCreateStream(tjsPath, TJS_BS_READ);
		if (st) {
			tjs_uint8 sig[4];
			if (st->Read(sig, 4) == 4 && sig[0] == 'X' && sig[1] == 'P')
				bootable = true;
			delete st;
		}
	} else {
	// Directory — look for startup.tjs case-insensitively
	TVPListDir(startupPath, [&](const std::string &_name, int mask) {
		if (bootable) return;
		std::string name = _name;
		for (auto &c : name) if (c >= 'A' && c <= 'Z') c += 0x20;
		if (name == "startup.tjs") bootable = true;
	});
	}

	// Parse -key=value arguments (TVPSystemOption is not available in this build)
	for (const std::string &arg : g_AndroidStartupArgs) {
		if (arg.size() > 1 && arg[0] == '-') {
			__android_log_print(ANDROID_LOG_INFO, TAG,
				"startup arg: %s", arg.c_str());
			// FIXME: set system options when TVPSetSystemOption is available
		}
	}

	if (bootable) {
		__android_log_print(ANDROID_LOG_INFO, TAG,
			"TVPCheckStartupArg: booting from %s", startupPath.c_str());
		LocaleConfigManager *mgr = LocaleConfigManager::GetInstance();
		if (mgr) mgr->Initialize(TVPGetCurrentLanguage());

		::Application->StartApplication(ttstr(startupPath));
		return true;
	}
	// No startup path given — auto-detect
	__android_log_print(ANDROID_LOG_INFO, TAG,
		"TVPCheckStartupArg: no path, scanning for games...");
	return false;
}

//---------------------------------------------------------------------------
// Android_PushEvents — thread-safe queue for JNI→main-thread callbacks
//---------------------------------------------------------------------------
static std::mutex g_eventQueueMutex;
static std::queue<std::function<void()>> g_eventQueue;

void Android_PushEvents(std::function<void()> func) {
	if (!func) return;
	std::lock_guard<std::mutex> lk(g_eventQueueMutex);
	g_eventQueue.push(std::move(func));
}

void DrainAndroidEventQueue() {
	// Swap the queue under lock, then drain outside lock to avoid deadlock if
	// a callback itself pushes to the queue.
	std::queue<std::function<void()>> local;
	{
		std::lock_guard<std::mutex> lk(g_eventQueueMutex);
		local.swap(g_eventQueue);
	}
	while (!local.empty()) {
		auto fn = std::move(local.front());
		local.pop();
		if (fn) fn();
	}
}

//---------------------------------------------------------------------------
// Android_Get*StoragePath
//---------------------------------------------------------------------------
std::vector<ttstr> Android_GetExternalStoragePath() {
	std::vector<ttstr> result;
	auto list = _getStoragePathList("getExternalStoragePath");
	if (!list.empty()) result.push_back(ttstr(list[0]));
	return result;
}
std::string Android_GetInternalStoragePath() {
	auto list = _getStoragePathList("getInternalStoragePath");
	return list.empty() ? "" : list[0];
}
std::string Android_GetDumpStoragePath() {
	auto list = _getStoragePathList("getInternalStoragePath");
	return list.empty() ? "" : list[0];
}
std::string Android_GetApkStoragePath() {
	auto list = _getStoragePathList("getApkStoragePath");
	return list.empty() ? "" : list[0];
}
