//---------------------------------------------------------------------------
// SDL3 Android JNI bridge — Phase 2: SDL_GL context + SDL3 input events
// SDL3's SDL_android.c owns the JNI_OnLoad and registers all SDLActivity
// JNI methods. We provide only KR2Activity-specific JNI functions (storage,
// SAF, message box, menu, dump, startup args) via JNI name resolution.
// SDL_main is the engine entry point from SDL3's nativeRunMain.
//---------------------------------------------------------------------------
#include <jni.h>
#include <android/log.h>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>

#include "tjsCommHead.h"
#include "Application.h"
#include "SysInitImpl.h"
#include "DebugIntf.h"
#include "sdl/TVPSDL.h"
#include "SDL3/SDL_main.h"
#include "environ/android/JNIHelper_sdl.h"
#include "ConfigManager/GlobalConfigManager.h"
#include "ConfigManager/LocaleConfigManager.h"
#include "Platform.h"
#include "environ/sdl/DebugLayer.h"

#include "breakpad/client/linux/handler/exception_handler.h"
#include "breakpad/client/linux/handler/minidump_descriptor.h"

#include "WindowLayer_sdl.h"
#include "visual/gpu/RenderManager_gpu.h"
#include "visual/win32/MenuItemImpl.h"
#include "vkdefine.h"
#include "sound/win32/WaveMixer.h"

extern void TVPForceSwapBuffer();
extern void TVPShowGamePicker();
iTJSDispatch2* TVPGetMenuDispatch(tTVInteger hWnd);

#define TAG "##krkr"

JavaVM *jni::g_JVM = nullptr;

// Display backend selection — set from Java before SDL_main runs.
// "vulkan" = Vulkan GPU display (SDL_ClaimWindowForGPUDevice)
// "sdl"    = SDL_Renderer display
std::string g_AndroidDisplayBackend = "vulkan"; // default

//---------------------------------------------------------------------------
// Log overlay — forwards TVPAddLog lines to Java LogOverlay via JNI
//---------------------------------------------------------------------------
static jclass    sLogOverlayCls   = nullptr;
static jmethodID sLogOverlayMethod = nullptr;

static void LogOverlayCallback(const ttstr &line) {
	if (!jni::g_JVM || !sLogOverlayMethod) return;
	JNIEnv *env = nullptr;
	bool needsDetach = false;
	int st = jni::g_JVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
	if (st == JNI_EDETACHED) {
		if (jni::g_JVM->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
		needsDetach = true;
	} else if (st != JNI_OK) {
		return;
	}
	std::string utf8 = line.AsNarrowStdString();
	jstring jline = env->NewStringUTF(utf8.c_str());
	env->CallStaticVoidMethod(sLogOverlayCls, sLogOverlayMethod, jline);
	env->DeleteLocalRef(jline);
	if (needsDetach) jni::g_JVM->DetachCurrentThread();
}

//---------------------------------------------------------------------------
// Helper: JNI string -> std::string, cache g_JVM from first JNI call
//---------------------------------------------------------------------------
static std::string jstr2str(JNIEnv *env, jstring jstr) {
	if (!jstr) return {};
	const char *utf = env->GetStringUTFChars(jstr, nullptr);
	if (!utf) return {};
	std::string ret(utf);
	env->ReleaseStringUTFChars(jstr, utf);
	return ret;
}

//---------------------------------------------------------------------------
// ===== JNI initialization helpers =====
// Called from Java (KR2Activity.onCreate) to set up g_JVM
//---------------------------------------------------------------------------
extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeInitJNI(JNIEnv *env, jclass) {
	if (!jni::g_JVM) {
		env->GetJavaVM(&jni::g_JVM);
	}
}

//---------------------------------------------------------------------------
// ===== KR2Activity (org.tvp.kirikiri2.KR2Activity) =====
//---------------------------------------------------------------------------

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_initDump(JNIEnv *env, jclass, jstring jPath) {
	const char *path = env->GetStringUTFChars(jPath, nullptr);
	if (path && *path) {
		static google_breakpad::MinidumpDescriptor desc(path);
		static google_breakpad::ExceptionHandler eh(desc, nullptr, nullptr,
			nullptr, true, -1);
	}
	env->ReleaseStringUTFChars(jPath, path);
}

namespace kr2android {
	std::condition_variable MessageBoxCond;
	std::mutex MessageBoxLock;
	int MsgBoxRet = -1;
	std::string MessageBoxRetText;

	std::condition_variable MenuCond;
	std::mutex MenuLock;
	int MenuRet = -2;
}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_onMessageBoxOK(JNIEnv *, jclass, jint btn) {
	kr2android::MsgBoxRet = btn;
	kr2android::MessageBoxCond.notify_one();
}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_onMessageBoxText(JNIEnv *env, jclass, jstring text) {
	kr2android::MessageBoxRetText = jstr2str(env, text);
}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_onNativeInit(JNIEnv *, jclass) {}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_onNativeExit(JNIEnv *, jclass) {}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_onBannerSizeChanged(JNIEnv *, jclass, jint, jint) {}

// --- KR2Activity SAF / config ---

extern "C" JNIEXPORT jstring JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeGetSafTreeUri(JNIEnv *env, jclass) {
	std::string uri = GlobalConfigManager::GetInstance()
		->GetValue<std::string>("saf_tree_uri", "");
	return env->NewStringUTF(uri.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeSetSafTreeUri(JNIEnv *env, jclass,
		jstring jUri) {
	std::string uri = jstr2str(env, jUri);
	Android_PushEvents([uri]() {
		GlobalConfigManager::GetInstance()->SetValue("saf_tree_uri", uri);
		GlobalConfigManager::GetInstance()->SaveToFile();
	});
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeGetHideSystemButton(JNIEnv *, jclass) {
	return GlobalConfigManager::GetInstance()
		->GetValue<bool>("hide_android_sys_btn", false);
}

// --- KR2Activity display backend ---

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeSetDisplayBackend(JNIEnv *env, jclass,
		jstring jBackend) {
	if (jBackend) {
		const char *s = env->GetStringUTFChars(jBackend, nullptr);
		if (s) {
			g_AndroidDisplayBackend = s;
			__android_log_print(ANDROID_LOG_INFO, TAG,
				"nativeSetDisplayBackend: %s", s);
			env->ReleaseStringUTFChars(jBackend, s);
		}
	}
}

// --- KR2Activity startup args ---

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeSetStartupArgs(JNIEnv *env, jclass,
		jstring startupPath, jobjectArray args) {
	if (startupPath) {
		g_AndroidStartupPath = jstr2str(env, startupPath);
	}
	jsize count = args ? env->GetArrayLength(args) : 0;
	g_AndroidStartupArgs.clear();
	for (jsize i = 0; i < count; ++i) {
		jstring arg = (jstring)env->GetObjectArrayElement(args, i);
		if (arg) {
			g_AndroidStartupArgs.push_back(jstr2str(env, arg));
			env->DeleteLocalRef(arg);
		}
	}
	__android_log_print(ANDROID_LOG_INFO, TAG,
		"nativeSetStartupArgs: path=%s (%zu args)",
		g_AndroidStartupPath.c_str(), g_AndroidStartupArgs.size());
}

//---------------------------------------------------------------------------
// In-game menu actions
//---------------------------------------------------------------------------
extern tTJSNI_Window *TVPGetActiveWindow();
extern void TVPShowIME(tjs_int x, tjs_int y, tjs_int w, tjs_int h);

bool g_mouseMode = true;
float g_cursorXf = 0, g_cursorYf = 0;
extern bool g_fullscreenStretch;

extern "C" JNIEXPORT jboolean JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeIsFullscreenStretch(JNIEnv*, jclass) {
	return g_fullscreenStretch ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeToggleAspectRatio(JNIEnv*, jclass) {
	g_fullscreenStretch = !g_fullscreenStretch;
}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeOnMenuResult(JNIEnv*, jclass, jint index) {
	kr2android::MenuRet = index;
	kr2android::MenuCond.notify_one();
}

//---------------------------------------------------------------------------
// TVPShowPopMenu — show TJS menu tree via native dialog (synchronous)
//---------------------------------------------------------------------------
void TVPShowPopMenu(tTJSNI_MenuItem *item) {
	if (!item) { __android_log_print(ANDROID_LOG_WARN, TAG, "TVPShowPopMenu: null"); return; }
	const auto &children = item->GetChildren();
	if (children.empty()) {
		item->OnClick();
		return;
	}
	size_t n = children.size();
	std::vector<std::string> captions;
	std::vector<bool> hasSub;
	captions.reserve(n); hasSub.reserve(n);
	for (size_t i = 0; i < n; i++) {
		const tTJSNI_MenuItem *child = static_cast<const tTJSNI_MenuItem*>(children[i]);
		ttstr cap; child->GetCaption(cap);
		if (cap.IsEmpty()) {
			captions.push_back("---");
		} else {
			if (child->GetChecked()) cap = ttstr(TJS_W("\u2713 ")) + cap;
			captions.push_back(cap.AsNarrowStdString());
		}
		hasSub.push_back(!child->GetChildren().empty());
	}
	JNIEnv *env = jni::GetEnv();
	if (!env) return;
	jclass cls = env->FindClass("org/tvp/kirikiri2/KR2Activity");
	if (!cls) return;
	jmethodID mid = env->GetStaticMethodID(cls, "ShowMenuDialog",
		"([Ljava/lang/String;[Z)V");
	if (!mid) { env->DeleteLocalRef(cls); return; }
	jclass scls = env->FindClass("java/lang/String");
	jobjectArray jCaptions = env->NewObjectArray(n, scls, nullptr);
	for (size_t i = 0; i < n; i++)
		env->SetObjectArrayElement(jCaptions, i, env->NewStringUTF(captions[i].c_str()));
	jbooleanArray jHasSub = env->NewBooleanArray(n);
	jboolean *jSubData = new jboolean[n];
	for (size_t i = 0; i < n; i++) jSubData[i] = hasSub[i] ? JNI_TRUE : JNI_FALSE;
	env->SetBooleanArrayRegion(jHasSub, 0, n, jSubData);
	delete[] jSubData;
	env->CallStaticVoidMethod(cls, mid, jCaptions, jHasSub);
	kr2android::MenuRet = -2;
	{
		std::unique_lock<std::mutex> lk(kr2android::MenuLock);
		while (kr2android::MenuRet == -2) {
			kr2android::MenuLock.unlock();
			TVPForceSwapBuffer();
			{ std::unique_lock<std::mutex> lk2(kr2android::MenuLock); (void)lk2; }
			kr2android::MenuLock.lock();
		}
	}
	int sel = kr2android::MenuRet;
	env->DeleteLocalRef(cls); env->DeleteLocalRef(scls);
	env->DeleteLocalRef(jCaptions); env->DeleteLocalRef(jHasSub);
	if (sel < 0 || (size_t)sel >= n) return;
	const tTJSNI_MenuItem *child = static_cast<const tTJSNI_MenuItem*>(children[sel]);
	if (hasSub[sel]) TVPShowPopMenu(const_cast<tTJSNI_MenuItem*>(child));
	else const_cast<tTJSNI_MenuItem*>(child)->OnClick();
}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeShowGameMenu(JNIEnv*, jclass) {
	Android_PushEvents([]() {
		tTJSNI_Window *win = TVPGetActiveWindow();
		if (!win) return;
		iTJSDispatch2 *menuobj = TVPGetMenuDispatch((tjs_intptr_t)win);
		if (!menuobj) return;
		tTJSNI_MenuItem *menu = nullptr;
		menuobj->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
			tTJSNC_MenuItem::ClassID, (iTJSNativeInstance**)&menu);
		if (!menu || menu->GetChildren().empty()) return;
		TVPShowPopMenu(menu);
	});
}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeToggleMouseMode(JNIEnv*, jclass) {
	g_mouseMode = !g_mouseMode;
}
extern "C" JNIEXPORT jboolean JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeGetMouseMode(JNIEnv*, jclass) {
	return (jboolean)g_mouseMode;
}
extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeSetMouseMode(JNIEnv*, jclass, jboolean on) {
	g_mouseMode = on;
}

void TVPUpdateCursorOverlay() {
	JNIEnv *env = jni::GetEnv();
	if (!env) return;
	env->ExceptionClear();

	// Sync visibility with g_mouseMode state changes
	static bool s_prevMouseMode = false;
	if (g_mouseMode != s_prevMouseMode) {
		s_prevMouseMode = g_mouseMode;
		jclass cls = env->FindClass("org/tvp/kirikiri2/KR2Activity");
		if (cls) {
			jmethodID mid = env->GetStaticMethodID(cls, "setCursorVisible", "(Z)V");
			if (mid) env->CallStaticVoidMethod(cls, mid, (jboolean)g_mouseMode);
			env->DeleteLocalRef(cls);
		}
	}

	if (!g_mouseMode) return;
	jclass cls = env->FindClass("org/tvp/kirikiri2/KR2Activity");
	if (!cls) return;
	jmethodID mid = env->GetStaticMethodID(cls, "setCursorPos", "(II)V");
	if (mid) {
		int cx = g_cursorX(), cy = g_cursorY();
		float scx = (float)cx, scy = (float)cy;
		TVPGameToScreen(scx, scy);
		env->CallStaticVoidMethod(cls, mid, (int)scx, (int)scy);
	}
	env->DeleteLocalRef(cls);
}

// Debug capture toggle — called from Java overlay button
// Triggers a single RenderDoc capture on the next frame.
extern void TriggerRenderDocCapture();

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeToggleCapture(JNIEnv*, jclass) {
	TriggerRenderDocCapture();
	__android_log_print(ANDROID_LOG_INFO, "##krkr", "RenderDoc capture toggled");
}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeShowKeyboard(JNIEnv*, jclass) {
	TVPShowIME(0, 0, 1920, 1080);
}

extern "C" JNIEXPORT void JNICALL
Java_org_tvp_kirikiri2_KR2Activity_nativeGameMenuExit(JNIEnv*, jclass) {
	Android_PushEvents([]() {
		tTJSNI_Window *w = TVPGetActiveWindow();
		if (w) w->Close();
	});
}

//---------------------------------------------------------------------------
// SDL_main — engine entry point (called by SDL3's nativeRunMain on SDLThread)
//---------------------------------------------------------------------------
extern "C" int SDL_main(int argc, char *argv[]) {
	__android_log_print(ANDROID_LOG_INFO, TAG, "SDL_main ENTER");

	// Cache JVM from SDL3's env
	JNIEnv *env = (JNIEnv*)SDL_GetAndroidJNIEnv();
	if (env) env->GetJavaVM(&jni::g_JVM);

	// Init SDL subsystems + engine
	TVPInitSDL();

	// Create window (no OPENGL flag — using Vulkan via SDL_Gpu or SDL_Renderer)
	SDL_Window *win = SDL_CreateWindow("Kirikiroid2Yuri", 1280, 720, 0);
	if (!win) {
		__android_log_print(ANDROID_LOG_ERROR, TAG,
			"SDL_CreateWindow failed: %s", SDL_GetError());
		return 1;
	}
	g_window = win;

	int scrW, scrH;
	SDL_GetWindowSize(win, &scrW, &scrH);
	TVPSetScreenSizeFromSDL(scrW, scrH);
	__android_log_print(ANDROID_LOG_INFO, TAG,
		"SDL_main: window %dx%d created", scrW, scrH);

	// Cache JNI class/method for log overlay forwarding
	if (jni::g_JVM) {
		JNIEnv *env = nullptr;
		jni::g_JVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
		if (!env) jni::g_JVM->AttachCurrentThread(&env, nullptr);
		if (env) {
			jclass cls = env->FindClass("org/tvp/kirikiri2/LogOverlay");
			if (cls) {
				sLogOverlayCls = (jclass)env->NewGlobalRef(cls);
				sLogOverlayMethod = env->GetStaticMethodID(cls, "appendLog", "(Ljava/lang/String;)V");
				env->DeleteLocalRef(cls);
			}
			if (sLogOverlayMethod) {
				TVPSetOnLogOverlay(LogOverlayCallback);
				__android_log_print(ANDROID_LOG_INFO, TAG, "Log overlay callback registered");
			}
		}
	}

	// Init display backend based on Java-side Intent preference.
	// Only ONE backend is initialized to avoid Vulkan window claim conflicts.
	// Vulkan: SDL_ClaimWindowForGPUDevice — full GPU compositing.
	// SDL: SDL_Renderer — software compositing only.
	bool gpuInitialized = false;
	if (g_AndroidDisplayBackend == "vulkan" || g_AndroidDisplayBackend == "gpu") {
		TVPRenderManager_GPU *gpuRenderer = new TVPRenderManager_GPU();
		if (gpuRenderer->Init(win)) {
			__android_log_print(ANDROID_LOG_INFO, TAG,
				"GPU renderer (Vulkan) initialized");
			TVPRetryGPU();
			gpuInitialized = true;
		} else {
			__android_log_print(ANDROID_LOG_WARN, TAG,
				"Vulkan init failed, falling back to SDL_Renderer");
			delete gpuRenderer;
		}
	}
	if (!gpuInitialized) {
		if (!TVPInitDisplay(win)) {
			__android_log_print(ANDROID_LOG_ERROR, TAG,
				"SDL_Renderer init failed, cannot render");
			return 1;
		}
		__android_log_print(ANDROID_LOG_INFO, TAG,
			"SDL_Renderer display initialized (software compositing)");
	}

	// Set global flag for render manager selection
	g_VulkanDisplayActive = gpuInitialized;

	// Start debug TCP server if configured
	{
		int debugPort = GlobalConfigManager::GetInstance()
			->GetValue<int>("debug_tcp_port", 0);
		if (debugPort > 0) {
			TVPDebugLayer::Instance()->StartServer(debugPort);
			__android_log_print(ANDROID_LOG_INFO, TAG,
				"Debug TCP server started on port %d", debugPort);
		}
	}

	// Init locale
	LocaleConfigManager::GetInstance()->Initialize(TVPGetCurrentLanguage());

	// Startup compositing — only if GPU is active (renderer config selects "gpu")
	if (TVPRenderManager_GPU::Instance() && !TVPGetRenderManager()->IsSoftware()) {
		TVPRenderManager_GPU::Instance()->BeginFrame();
		// Check startup args or auto-start game
		if (!TVPCheckStartupArg()) {
			TVPShowGamePicker();
		}
		TVPRenderManager_GPU::Instance()->EndFrame();
	} else {
		// Check startup args or auto-start game (software mode)
		if (!TVPCheckStartupArg()) {
			TVPShowGamePicker();
		}
	}

	// Main render loop
	while (!::Application->IsTarminate()) {
		TVPEngineTick();
	}

	// Cleanup — keep SDL alive so process stays for LauncherActivity
	__android_log_print(ANDROID_LOG_INFO, TAG, "SDL_main: engine terminated, cleanup");
	TVPUninitDirectSound();
	SDL_DestroyWindow(win);
	__android_log_print(ANDROID_LOG_INFO, TAG, "SDL_main EXIT");
	return 0;
}
