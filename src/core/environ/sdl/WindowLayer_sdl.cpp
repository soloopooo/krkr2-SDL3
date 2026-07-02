#include <SDL3/SDL.h>
#include <SDL3/SDL_render.h>
#include <android/log.h>
#include <chrono>
#include <thread>
#include <string>
#include <cstdint>
#include <cmath>
#include <algorithm>

#include "../android/JNIHelper_sdl.h"

bool g_fullscreenStretch = false;
bool g_VulkanDisplayActive = false;
#include <vector>

#include "WindowLayer_sdl.h"
#include "tjsCommHead.h"
#include "Application.h"
#include "TickCount.h"
#include "EventIntf.h"
#include "tvpinputdefs.h"
#include "Random.h"
#include "vkdefine.h"
#include "sdl/TVPSDL.h"
#include "environ/android/JNIHelper_sdl.h"
#include "RenderManager.h"
#include "WindowIntf.h"
#include "visual/win32/DrawDevice.h"
#include "visual/LayerManager.h"
#include "visual/LayerIntf.h"
#include "visual/gpu/RenderManager_gpu.h"
#include "ConfigManager/GlobalConfigManager.h"
#include "Platform.h"

#define TAG "##krkr"

extern tjs_uint32 TVPGetCurrentShiftKeyState();
extern void TVPForceSwapBuffer();
void TVPEngineTick();

//------------------------------------------------------------------------------
// Display mode
//------------------------------------------------------------------------------
static SDL_Renderer *s_renderer = nullptr;   // Software mode display
static SDL_Texture *s_swDispTex = nullptr;
static int s_swDispW = 0, s_swDispH = 0;

bool TVPInitDisplay(SDL_Window *win) {
	if (s_renderer) return true;
	s_renderer = SDL_CreateRenderer(win, NULL);
	if (!s_renderer) {
		__android_log_print(ANDROID_LOG_ERROR, TAG,
			"SDL_CreateRenderer: %s", SDL_GetError());
		return false;
	}
	__android_log_print(ANDROID_LOG_INFO, TAG, "SDL_Renderer created for software display");
	return true;
}

void TVPForceSwapBuffer() {} // No-op: display updates per-frame
//------------------------------------------------------------------------------
// TVPWindowLayerSDL
//------------------------------------------------------------------------------
static tjs_uint8 s_Scancode[0x200];
static tjs_uint16 s_Keymap[0x200];
int s_ScreenWidth = 0, s_ScreenHeight = 0;
int g_bufferOffsetX = 0; // pixel read offset from buffer centering (lockTouchSize)

TVPWindowLayerSDL *TVPWindowLayerSDL::s_ActiveWindow = nullptr;
TVPWindowLayerSDL *TVPWindowLayerSDL::s_LastWindow = nullptr;

TVPWindowLayerSDL::TVPWindowLayerSDL(tTJSNI_Window *w)
	: m_Window(w), m_Visible(false), m_Width(0), m_Height(0)
	, m_LastMouseX(0), m_LastMouseY(0), m_InModal(false), m_ModalResult(0)
	, m_ZoomNumer(1), m_ZoomDenom(1), m_UseMouseKey(false)
	, m_MouseLeftEmulated(false), m_MouseRightEmulated(false)
	, m_LastMouseKeyTick(0), m_MouseKeyXAccel(0), m_MouseKeyYAccel(0)
	, m_Prev(nullptr), m_Next(nullptr) {
	m_LastMouseKeyTick = TVPGetRoughTickCount32();
	m_Prev = s_LastWindow; s_LastWindow = this;
	if (m_Prev) m_Prev->m_Next = this;
}

TVPWindowLayerSDL::~TVPWindowLayerSDL() {
	if (s_LastWindow == this) s_LastWindow = m_Prev;
	if (m_Next) m_Next->m_Prev = m_Prev;
	if (m_Prev) m_Prev->m_Next = m_Next;
	if (s_ActiveWindow == this) s_ActiveWindow = m_Prev ? m_Prev : m_Next;
}

// Fixed game resolution (set once from initial TJS Window constructor, never changes)
static int s_fixedGameW = 0, s_fixedGameH = 0;

void TVPWindowLayerSDL::SetPaintBoxSize(tjs_int w, tjs_int h) {
	if (g_gameW <= 0) {
		g_gameW = w; g_gameH = h;
		if (s_fixedGameW <= 0) { s_fixedGameW = w; s_fixedGameH = h; }
		m_Width = w; m_Height = h;
	} else {
		m_Width = g_gameW; m_Height = g_gameH;
	}
}
bool TVPWindowLayerSDL::GetFormEnabled() { return m_Visible; }
void TVPWindowLayerSDL::SetDefaultMouseCursor() {}
void TVPWindowLayerSDL::GetCursorPos(tjs_int &x, tjs_int &y) { x = m_LastMouseX; y = m_LastMouseY; }
void TVPWindowLayerSDL::SetCursorPos(tjs_int x, tjs_int y) { m_LastMouseX = x; m_LastMouseY = y; }
void TVPWindowLayerSDL::SetHintText(const ttstr &) {}
void TVPWindowLayerSDL::SetAttentionPoint(tjs_int, tjs_int, const struct tTVPFont *) {}
void TVPWindowLayerSDL::ZoomRectangle(tjs_int &l, tjs_int &t, tjs_int &r, tjs_int &b) {
	l = (tjs_int64)l * m_ZoomNumer / m_ZoomDenom; t = (tjs_int64)t * m_ZoomNumer / m_ZoomDenom;
	r = (tjs_int64)r * m_ZoomNumer / m_ZoomDenom; b = (tjs_int64)b * m_ZoomNumer / m_ZoomDenom;
}
void TVPWindowLayerSDL::BringToFront() { s_ActiveWindow = this; }

void TVPWindowLayerSDL::ShowWindowAsModal() {
	m_InModal = true; m_Visible = true; BringToFront(); m_ModalResult = 0;
	while (this == s_ActiveWindow && !m_ModalResult) {
		TVPEngineTick();
		if (::Application->IsTarminate()) { m_ModalResult = 1; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	m_InModal = false;
}

bool TVPWindowLayerSDL::GetVisible() { return m_Visible; }
void TVPWindowLayerSDL::SetVisible(bool b) { m_Visible = b; if (b) BringToFront(); }
const char *TVPWindowLayerSDL::GetCaption() { return m_Caption.c_str(); }
void TVPWindowLayerSDL::SetCaption(const std::string &s) { m_Caption = s; }
void TVPWindowLayerSDL::SetWidth(tjs_int w) { if (g_gameW > 0) w = g_gameW; m_Width = w; }
void TVPWindowLayerSDL::SetHeight(tjs_int h) { if (g_gameH > 0) h = g_gameH; m_Height = h; }
void TVPWindowLayerSDL::SetSize(tjs_int w, tjs_int h) { SetPaintBoxSize(w, h); }
void TVPWindowLayerSDL::GetSize(tjs_int &w, tjs_int &h) { w = m_Width; h = m_Height; }
tjs_int TVPWindowLayerSDL::GetWidth() const { return m_Width; }
tjs_int TVPWindowLayerSDL::GetHeight() const { return m_Height; }
void TVPWindowLayerSDL::GetWinSize(tjs_int &w, tjs_int &h) { w = s_ScreenWidth; h = s_ScreenHeight; }
void TVPWindowLayerSDL::SetZoom(tjs_int numer, tjs_int denom) { m_ZoomNumer = numer; m_ZoomDenom = denom; }
void TVPWindowLayerSDL::UpdateDrawBuffer(iTVPTexture2D *) {}

void TVPWindowLayerSDL::InvalidateClose() {
	if (m_Window) { iTJSDispatch2 *obj = m_Window->GetOwnerNoAddRef(); obj->Invalidate(0, nullptr, nullptr, obj); m_Window = nullptr; }
	delete this;
}
bool TVPWindowLayerSDL::GetWindowActive() { return s_ActiveWindow == this; }

void TVPWindowLayerSDL::Close() {
	if (m_InModal) { m_ModalResult = 1; return; }
	if (!m_Window) return;
	iTJSDispatch2 *obj = m_Window->GetOwnerNoAddRef();
	if (obj) {
		static ttstr eventname(TJS_W("onCloseQuery"));
		tTJSVariant arg = true;
		TVPPostEvent(obj, obj, eventname, 0, TVP_EPT_IMMEDIATE, 1, &arg);
	}
}

void TVPWindowLayerSDL::OnCloseQueryCalled(bool b) {
	if (!b || !m_Window) return;
	iTJSDispatch2 *obj = m_Window->GetOwnerNoAddRef();
	if (obj) obj->Invalidate(0, nullptr, nullptr, obj);
	m_Window = nullptr; s_ActiveWindow = nullptr;
}

void TVPWindowLayerSDL::InternalKeyDown(tjs_uint16 key, tjs_uint32 shift) {
	if (!m_Window) return;
	tjs_uint32 tick = TVPGetRoughTickCount32();
	TVPPushEnvironNoise(&tick, sizeof(tick)); TVPPushEnvironNoise(&key, sizeof(key)); TVPPushEnvironNoise(&shift, sizeof(shift));
	if (m_UseMouseKey) {
		if (key == VK_RETURN || key == VK_SPACE || key == VK_ESCAPE || key == VK_PAD1 || key == VK_PAD2) {
			if (m_LastMouseX >= 0 && m_LastMouseY >= 0 && m_LastMouseX < m_Width && m_LastMouseY < m_Height) {
				if (key == VK_RETURN || key == VK_SPACE || key == VK_PAD1) m_MouseLeftEmulated = true;
				if (key == VK_ESCAPE || key == VK_PAD2) m_MouseRightEmulated = true;
			}
			return;
		}
	}
	TVPPostInputEvent(new tTVPOnKeyDownInputEvent(m_Window, key, shift));
}

void TVPWindowLayerSDL::OnKeyUp(tjs_uint16 vk, int shift) {
	if (m_Window) TVPPostInputEvent(new tTVPOnKeyUpInputEvent(m_Window, vk, shift));
}
void TVPWindowLayerSDL::OnKeyPress(tjs_uint16 vk, int, bool, bool) {
	if (m_Window && vk) TVPPostInputEvent(new tTVPOnKeyPressInputEvent(m_Window, vk));
}
tTVPImeMode TVPWindowLayerSDL::GetDefaultImeMode() const { return imDisable; }
void TVPWindowLayerSDL::SetImeMode(tTVPImeMode) {}
void TVPWindowLayerSDL::ResetImeMode() {}

void TVPWindowLayerSDL::UpdateWindow(tTVPUpdateType) {
	if (m_Window) { m_Window->NotifyWindowExposureToLayer({0,0,m_Width,m_Height}); TVPDeliverWindowUpdateEvents(); }
}
void TVPWindowLayerSDL::SetVisibleFromScript(bool b) { SetVisible(b); }
void TVPWindowLayerSDL::SetUseMouseKey(bool b) { m_UseMouseKey = b; }
bool TVPWindowLayerSDL::GetUseMouseKey() const { return m_UseMouseKey; }
void TVPWindowLayerSDL::ResetMouseVelocity() {}
void TVPWindowLayerSDL::ResetTouchVelocity(tjs_int) {}
bool TVPWindowLayerSDL::GetMouseVelocity(float &, float &, float &) const { return false; }

void TVPWindowLayerSDL::TickBeat() {
	if (m_UseMouseKey && s_ActiveWindow == this) GenerateMouseEvent(false,false,false,false);
}

void TVPWindowLayerSDL::GenerateMouseEvent(bool fl, bool fr, bool fu, bool fd) {
	if (!fl && !fr && !fu && !fd && TVPGetRoughTickCount32() - 45 < m_LastMouseKeyTick) return;
	bool left = fl || (s_Scancode[VK_LEFT] & 1), right = fr || (s_Scancode[VK_RIGHT] & 1);
	bool up = fu || (s_Scancode[VK_UP] & 1), down = fd || (s_Scancode[VK_DOWN] & 1);
	if (!right && !left && !up && !down) { m_MouseKeyXAccel = m_MouseKeyYAccel = 0; }
	if (left) if (m_MouseKeyXAccel > -30) m_MouseKeyXAccel -= 2;
	if (right) if (m_MouseKeyXAccel < 30) m_MouseKeyXAccel += 2;
	if (up) if (m_MouseKeyYAccel > -30) m_MouseKeyYAccel -= 2;
	if (down) if (m_MouseKeyYAccel < 30) m_MouseKeyYAccel += 2;
	m_LastMouseX += m_MouseKeyXAccel >> 1; m_LastMouseY += m_MouseKeyYAccel >> 1;
	m_LastMouseKeyTick = TVPGetRoughTickCount32();
}

//------------------------------------------------------------------------------
// Global functions
//------------------------------------------------------------------------------
iWindowLayer *TVPCreateAndAddWindow(tTJSNI_Window *w) {
	auto *win = new TVPWindowLayerSDL(w);
	// Paint box will be set by TJS Window constructor
	return win;
}
void TVPRemoveWindowLayer(iWindowLayer *lay) { delete static_cast<TVPWindowLayerSDL *>(lay); }
tTJSNI_Window *TVPGetActiveWindow() {
	auto *w = TVPWindowLayerSDL::GetActiveWindow(); return w ? w->GetWindow() : nullptr;
}
bool TVPGetScreenSize(tjs_int idx, tjs_int &w, tjs_int &h) {
	if (idx != 0) return false; w = s_ScreenWidth; h = s_ScreenHeight; return true;
}
ttstr TVPGetDataPath() {
	extern std::string Android_GetInternalStoragePath();
	std::string path = Android_GetInternalStoragePath();
	if (path.empty()) path = "/data/data/com.yuri.kirikiri2/files/";
	return ttstr(path);
}
tjs_uint32 TVPGetCurrentShiftKeyState() {
	tjs_uint32 f = 0;
	if (s_Scancode[VK_SHIFT] & 1) f |= ssShift;
	if (s_Scancode[VK_MENU] & 1) f |= ssAlt;
	if (s_Scancode[VK_CONTROL] & 1) f |= ssCtrl;
	if (s_Scancode[VK_LBUTTON] & 1) f |= ssLeft;
	if (s_Scancode[VK_RBUTTON] & 1) f |= ssRight;
	return f;
}
void TVPConsoleLog(const ttstr &l, bool) {
	__android_log_print(ANDROID_LOG_INFO, TAG, "%s", l.AsNarrowStdString().c_str());
	// Also forward to Java-side log buffer for in-app log viewer
	JNIEnv *env = jni::GetEnv();
	if (!env) return;
	env->ExceptionClear();
	jclass cls = env->FindClass("org/tvp/kirikiri2/KR2Activity");
	if (!cls) return;
	jmethodID mid = env->GetStaticMethodID(cls, "addEngineLog", "(Ljava/lang/String;)V");
	if (mid) {
		std::string s = l.AsNarrowStdString();
		jstring js = env->NewStringUTF(s.c_str());
		env->CallStaticVoidMethod(cls, mid, js);
		env->DeleteLocalRef(js);
	}
	env->DeleteLocalRef(cls);
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
static int s_dumpFrame = 0;
static void DumpBMP(const uint32_t *pixels, int w, int h) {
	if (!pixels || w <= 0 || h <= 0) return;
	// Dump every 30th frame (for a ~60fps game, that's every ~500ms)
	s_dumpFrame++;
	if (s_dumpFrame % 30 != 0) return;
	int pitch = w * 4;
	int dataSize = pitch * h;
	int bmpSize = 14 + 40 + dataSize;
	std::vector<uint8_t> bmp(bmpSize);
	// BMP header
	bmp[0] = 'B'; bmp[1] = 'M';
	*(uint32_t*)&bmp[2] = bmpSize;
	*(uint32_t*)&bmp[10] = 14 + 40;
	// DIB header
	*(uint32_t*)&bmp[14] = 40;        // header size
	*(int32_t*) &bmp[18] = w;         // width
	*(int32_t*) &bmp[22] = -h;        // negative height = top-down
	*(uint16_t*)&bmp[26] = 1;         // planes
	*(uint16_t*)&bmp[28] = 32;        // bpp
	*(uint32_t*)&bmp[30] = 0;         // no compression
	*(uint32_t*)&bmp[34] = dataSize;  // image size
	// Pixel data (ABGR → BGRA with alpha=255)
	auto *dst = &bmp[54];
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint32_t px = pixels[y * w + x];
			dst[0] = (uint8_t)(px >> 16); // B
			dst[1] = (uint8_t)(px >> 8);  // G
			dst[2] = (uint8_t)(px);       // R
			dst[3] = 0xFF;                // A
			dst += 4;
		}
	}
	char path[128];
	snprintf(path, sizeof(path), "/sdcard/Download/dump_%03d.bmp", s_dumpFrame/30);
	FILE *f = fopen(path, "wb");
	if (f) { fwrite(bmp.data(), 1, bmpSize, f); fclose(f); }
	__android_log_print(ANDROID_LOG_INFO, TAG, "DUMP: %s (%dx%d)", path, w, h);
}
// Per-frame engine tick
//------------------------------------------------------------------------------
static struct { int w, h; std::vector<uint32_t> pix; } g_frameBuf;
int g_gameW = 0, g_gameH = 0;
static struct {
	std::chrono::steady_clock::time_point lastLog;
	int frameCount, framesWithDraws, drawCallCount;
	long long tickTotalUs;
} g_stats = { std::chrono::steady_clock::now(), 0, 0, 0, 0 };

//------------------------------------------------------------------------------
// Debug overlay: 8x8 bitmap font + FPS/memory rendering
//------------------------------------------------------------------------------
static const uint8_t font8x8[96][8] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ! unused
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // "
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // #
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // $
	{0x3C,0x24,0x3C,0x18,0x3C,0x24,0x3C,0x00}, // %  (0x25)
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // &
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '
	{0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, // ( left paren  (0x28)
	{0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00}, // ) right paren (0x29)
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // *
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // +
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ,
	{0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00}, // - hyphen
	{0x00,0x00,0x00,0x00,0x00,0x30,0x30,0x00}, // . (0x2E)
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // /
	{0x3C,0x66,0x6E,0x7E,0x76,0x66,0x3C,0x00}, // 0 (0x30)
	{0x18,0x38,0x18,0x18,0x18,0x18,0x3C,0x00}, // 1
	{0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, // 2
	{0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, // 3
	{0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00}, // 4
	{0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, // 5
	{0x3C,0x66,0x60,0x7C,0x66,0x66,0x3C,0x00}, // 6
	{0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, // 7
	{0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, // 8
	{0x3C,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00}, // 9
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // :
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ;
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // <
	{0x00,0x00,0x00,0x7E,0x00,0x7E,0x00,0x00}, // =
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // >
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ?
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // @
	{0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // A (0x41)
	{0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, // B (0x42)
	{0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // C (0x43)
	{0x7C,0x66,0x66,0x66,0x66,0x66,0x7C,0x00}, // D (0x44)
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // E
	{0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, // F (0x46)
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // G
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // H
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // I
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // J
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // K
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // L
	{0x66,0x66,0x66,0x66,0x66,0x7E,0x3C,0x00}, // M (0x4D)
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // N
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // O
	{0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // P (0x50)
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Q
	{0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00}, // R (0x52)
	{0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, // S (0x53)
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // T
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // U
	{0x66,0x66,0x66,0x3C,0x3C,0x18,0x18,0x00}, // V (0x56)
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // W (0x57)
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // X
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Y
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // Z
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // [
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // backslash
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ]
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ^
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // _
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // `
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // a
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // b
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // c
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // d
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // e
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // f
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // g
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // h
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // i
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // j
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // k
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // l
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // m
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // n
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // o
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // p
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // q
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // r
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // s
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // t
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // u
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // v
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // w
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // x
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // y
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // z
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // |
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // }
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ~
};

static void _drawText(uint32_t *pixels, int fbW, int fbH, int x, int y,
	const char *text, uint32_t color, int scale);

static void _updateDebugOverlayJNI(bool firstCheck, int activeDraws = 0, uint64_t vramSize = 0) {
	static bool showFps = false;
	static bool checked = false;
	if (!checked || firstCheck) {
		showFps = GlobalConfigManager::GetInstance()
			->GetValue<bool>("showfps", false);
		checked = true;
		// Show/hide the Android overlay via JNI
		JNIEnv *env = jni::GetEnv();
		if (env) {
			jclass cls = env->FindClass("org/tvp/kirikiri2/KR2Activity");
			if (cls) {
				jmethodID mid = env->GetStaticMethodID(cls, "showDebugOverlay", "(Z)V");
				if (mid) env->CallStaticVoidMethod(cls, mid, (jboolean)showFps);
				env->DeleteLocalRef(cls);
			}
		}
	}
	if (!showFps) return;

	// Instant FPS (1 / frame delta)
	static auto lastFpsTime = std::chrono::steady_clock::now();
	auto now = std::chrono::steady_clock::now();
	float dtSec = std::chrono::duration_cast<std::chrono::microseconds>(
		now - lastFpsTime).count() / 1000000.0f;
	lastFpsTime = now;
	float instFps = (dtSec > 0.0f) ? (1.0f / dtSec) : 0.0f;

	char text[128];
	snprintf(text, sizeof(text), "%.0f (%d draws)\n%d MB(%.2f MB) %d MB",
		instFps, activeDraws, TVPGetSelfUsedMemory(), (float)(vramSize >> 10) / 1024.0f, TVPGetSystemFreeMemory());

	// Update Android overlay via JNI
	JNIEnv *env = jni::GetEnv();
	if (env) {
		jclass cls = env->FindClass("org/tvp/kirikiri2/KR2Activity");
		if (cls) {
			jmethodID mid = env->GetStaticMethodID(cls, "updateDebugOverlay", "(Ljava/lang/String;)V");
			if (mid) {
				jstring jtext = env->NewStringUTF(text);
				env->CallStaticVoidMethod(cls, mid, jtext);
				env->DeleteLocalRef(jtext);
			}
			env->DeleteLocalRef(cls);
		}
	}
}

static void _drawText(uint32_t *pixels, int fbW, int fbH, int x, int y,
	const char *text, uint32_t color, int scale) {
	if (!pixels || !text) return;
	if (scale < 1) scale = 1;
	for (const char *p = text; *p && x < fbW; p++) {
		unsigned char ch = (unsigned char)(*p - 32);
		if (ch >= 96) ch = 0;
		int chW = 6 * scale, chH = 8 * scale;
		if (x + chW <= 0) { x += chW; continue; }
		for (int row = 0; row < 8; row++) {
			uint8_t bits = font8x8[ch][row];
			for (int col = 0; col < 6; col++) {
				if (!(bits & (0x20 >> col))) continue;
				for (int sy = 0; sy < scale; sy++) {
					int py = y + row * scale + sy;
					if (py < 0 || py >= fbH) continue;
					for (int sx = 0; sx < scale; sx++) {
						int px = x + col * scale + sx;
						if (px < 0 || px >= fbW) continue;
						pixels[py * fbW + px] = color;
					}
				}
			}
		}
		x += chW;
	}
}

void TVPEngineTick() {
	static bool firstFrame = true;
	if (firstFrame) {
		_updateDebugOverlayJNI(true);
		firstFrame = false;
	}
	auto tickStart = std::chrono::steady_clock::now();

	DrainAndroidEventQueue();
	TVPProcessSDLEvents();

	bool gpuActive = !TVPGetRenderManager()->IsSoftware();
	if (gpuActive) {
		TVPRenderManager_GPU::Instance()->BeginFrame();
	}

	::Application->Run();

	// After compositing, force locked size to game resolution (overrides TJS lockTouchSize)
	if (s_fixedGameW > 0) {
		TVPWindowLayerSDL *swin = TVPWindowLayerSDL::GetActiveWindow();
		if (swin) {
			tTJSNI_Window *wjs = swin->GetWindow();
			iTVPDrawDevice *dd = wjs ? wjs->GetDrawDevice() : nullptr;
			if (dd) static_cast<tTVPDrawDevice*>(dd)->SetLockedSize(s_fixedGameW, s_fixedGameH);
		}
	}
	iTVPTexture2D::RecycleProcess();
	// Deliver continuous events (calls transition idle callbacks, TJS continuous handlers)
	{
		static uint64_t s_logTick = 0;
		uint64_t engTick = (uint64_t)TVPGetTickCount();
		if (engTick - s_logTick >= 1000) {
			s_logTick = engTick;
			__android_log_print(ANDROID_LOG_INFO, "##krkr", "TICK: engTick=%llu", (unsigned long long)engTick);
		}
	}
	TVPDeliverContinuousEvent();
	TVPDeliverWindowUpdateEvents();

	// Force locked size to game resolution (overrides TJS lockTouchSize)
	if (s_fixedGameW > 0) {
		TVPWindowLayerSDL *swin = TVPWindowLayerSDL::GetActiveWindow();
		if (swin) {
			tTJSNI_Window *wjs = swin->GetWindow();
			iTVPDrawDevice *dd = wjs ? wjs->GetDrawDevice() : nullptr;
			if (dd) static_cast<tTVPDrawDevice*>(dd)->SetLockedSize(s_fixedGameW, s_fixedGameH);
		}
	}

	int activeDraws = 0;
	uint64_t vramSize = 0;
	if (gpuActive) {
		TVPRenderManager_GPU *gpu = TVPRenderManager_GPU::Instance();
		gpu->EndFrame();
		// Readback pixels for diagnostics
		int rw = 0, rh = 0;
		const uint8_t *rp = gpu->GetFramePixels(rw, rh);
		if (rp && rw > 0 && rh > 0) {
			g_gameW = rw; g_gameH = rh;
			if (rw != g_frameBuf.w || rh != g_frameBuf.h) {
				g_frameBuf.w = rw; g_frameBuf.h = rh;
				g_frameBuf.pix.resize(rw * rh, 0);
			}
			memcpy(g_frameBuf.pix.data(), rp, rw * rh * 4);
		}
		gpu->ResetFrameResult();
		// Dump every 30th frame
		{
			const uint32_t *dumpSrc = nullptr;
			int dumpW = 0, dumpH = 0;
			if (g_frameBuf.w > 0 && g_frameBuf.pix.size() > 0) {
				dumpSrc = g_frameBuf.pix.data();
				dumpW = g_frameBuf.w; dumpH = g_frameBuf.h;
			} else {
				// Fallback: read from DrawBuffer directly
				TVPWindowLayerSDL *swin = TVPWindowLayerSDL::GetActiveWindow();
				if (swin) {
					tTJSNI_Window *wjs = swin->GetWindow();
					iTVPDrawDevice *dd = wjs ? wjs->GetDrawDevice() : nullptr;
					if (dd) {
						auto *ddc = static_cast<tTVPDrawDevice*>(dd);
						for (size_t i = 0; ; i++) {
							iTVPLayerManager *lm = ddc->GetLayerManagerAt(i);
							if (!lm) break;
							iTVPBaseBitmap *dbuf = lm->GetDrawBuffer();
							if (dbuf && dbuf->GetWidth() > 0 && dbuf->GetHeight() > 0) {
								const void *sl = dbuf->GetScanLine(0);
								if (sl) {
									dumpW = (int)dbuf->GetWidth();
									dumpH = (int)dbuf->GetHeight();
									// Ensure g_frameBuf has the data
									if (dumpW != g_frameBuf.w || dumpH != g_frameBuf.h) {
										g_frameBuf.w = dumpW; g_frameBuf.h = dumpH;
										g_frameBuf.pix.resize(dumpW * dumpH, 0);
									}
									int pitch = (int)dbuf->GetPitchBytes();
									for (int y = 0; y < dumpH; y++)
										memcpy((uint8_t*)g_frameBuf.pix.data() + y * dumpW * 4,
											(const uint8_t*)sl + pitch * y, dumpW * 4);
									dumpSrc = g_frameBuf.pix.data();
								}
							}
						}
					}
				}
			}
			if (dumpSrc && dumpW > 0 && dumpH > 0) {
				s_dumpFrame++;
				if (s_dumpFrame % 30 == 0)
					DumpBMP(dumpSrc, dumpW, dumpH);
			}
		}
		unsigned int dc = 0; uint64_t vm = 0;
		TVPGetRenderManager()->GetRenderStat(dc, vm);
		activeDraws = (int)dc; vramSize = vm;
	} else {
		// Software path: read pixels from DrawBuffer, display via SDL_Renderer
		const void *pxData = nullptr;
		int pxPitch = 0, pxW = 0, pxH = 0;
		TVPWindowLayerSDL *sdlWin = TVPWindowLayerSDL::GetActiveWindow();
		if (sdlWin) {
			tTJSNI_Window *tjsWin = sdlWin->GetWindow();
			iTVPDrawDevice *dd = tjsWin ? tjsWin->GetDrawDevice() : nullptr;
			if (dd) {
				unsigned int dc = 0; uint64_t vm = 0;
				TVPGetRenderManager()->GetRenderStat(dc, vm);
				activeDraws = (int)dc; vramSize = vm;
				auto *ddc = static_cast<tTVPDrawDevice*>(dd);
				iTVPBaseBitmap *drawBuf = nullptr;
				static int s_lmFrame = 0; s_lmFrame++;
				for (size_t i = 0; ; i++) {
					iTVPLayerManager *lm = ddc->GetLayerManagerAt(i);
					if (!lm) break;
					iTVPBaseBitmap *buf = lm->GetDrawBuffer();
					if (buf) {
						if (s_lmFrame % 30 == 0) {
							const void *sl0 = buf->GetScanLine(0);
							uint32_t pix = sl0 ? *(const uint32_t*)sl0 : 0;
							__android_log_print(ANDROID_LOG_INFO, "##LM", "lm[%zu]=%p buf=%p %dx%d pix=0x%08X draws=%d",
								i, lm, buf, (int)buf->GetWidth(), (int)buf->GetHeight(), pix, activeDraws);
						}
						drawBuf = buf;
					}
				}
				if (drawBuf) {
					pxW = (int)drawBuf->GetWidth();
					pxH = (int)drawBuf->GetHeight();
					int pitched = (int)drawBuf->GetPitchBytes();
					const void *pxbuf = drawBuf->GetScanLine(0);
					if (pxbuf && pxW > 0 && pxH > 0) {
					int dispW = pxW;
					int dispH = pxH;
					if (dispW != g_frameBuf.w || dispH != g_frameBuf.h) {
						g_frameBuf.w = dispW; g_frameBuf.h = dispH;
						g_frameBuf.pix.resize(dispW * dispH, 0);
					}
					auto *dst = g_frameBuf.pix.data();
					int copyW = pxW;
					int copyH = pxH;
					// Copy full buffer and force alpha to opaque (fix: fade-out overlay clears Dst Alpha)
					for (int y = 0; y < copyH; y++) {
						memcpy((uint8_t*)dst + y * dispW * 4, (const uint8_t*)pxbuf + pitched * y, copyW * 4);
						uint32_t *row = (uint32_t*)dst + y * dispW;
						for (int x = 0; x < copyW; x++) row[x] |= 0xFF000000;
					}
					pxData = dst;
					pxPitch = dispW * 4;
					pxW = dispW; pxH = dispH;
					}
				}
			}
		}
		if (!pxData) {
			g_gameW = s_ScreenWidth ? s_ScreenWidth : 1280;
			g_gameH = s_ScreenHeight ? s_ScreenHeight : 720;
			int w = g_gameW, h = g_gameH;
			if (w != g_frameBuf.w || h != g_frameBuf.h) {
				g_frameBuf.w = w; g_frameBuf.h = h;
				g_frameBuf.pix.resize(w * h, 0);
			}
			auto *p = g_frameBuf.pix.data();
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
					*p++ = 0xFF000000 | (128 << 16) | ((uint8_t)(y*255/h) << 8) | (uint8_t)(x*255/w);
			pxData = g_frameBuf.pix.data();
			pxPitch = w * 4; pxW = w; pxH = h;
		}
		g_gameW = pxW; g_gameH = pxH;

		if (pxData && s_renderer && pxW > 0 && pxH > 0 &&
			(activeDraws > 0 || !s_swDispTex)) {
			// Detect pending buffer resize (primaryLayer.setSize from fullscreen)
			// On the first frame after a resize the buffer is all-zero → skip update
			static int s_lastPxW = 0, s_lastPxH = 0;
			bool bufferResized = (s_lastPxW > 0) && (pxW != s_lastPxW || pxH != s_lastPxH);
			s_lastPxW = pxW; s_lastPxH = pxH;

			// Use raw buffer size for display texture
			int dispW = pxW;
			int dispH = pxH;
			if (!s_swDispTex || s_swDispW != pxW || s_swDispH != pxH) {
				if (s_swDispTex) SDL_DestroyTexture(s_swDispTex);
				s_swDispTex = SDL_CreateTexture(s_renderer,
					SDL_PIXELFORMAT_ABGR8888,
					SDL_TEXTUREACCESS_STREAMING, pxW, pxH);
				s_swDispW = pxW; s_swDispH = pxH;
			}
			if (s_swDispTex) {
				if (!SDL_UpdateTexture(s_swDispTex, NULL, pxData, pxPitch))
					__android_log_print(ANDROID_LOG_ERROR, TAG, "SDL_UpdateTexture failed: %s", SDL_GetError());
				int outW, outH;
				SDL_GetRenderOutputSize(s_renderer, &outW, &outH);
				if (outW != s_ScreenWidth || outH != s_ScreenHeight)
					TVPSetScreenSizeFromSDL(outW, outH);
				SDL_Rect dst;
				if (g_fullscreenStretch) {
					dst.x = 0; dst.y = 0;
					dst.w = outW; dst.h = outH;
				} else {
					float gameAspect = (float)pxW / pxH;
					float screenAspect = (float)outW / outH;
					if (gameAspect > screenAspect) {
						dst.w = outW;
						dst.h = (int)(outW / gameAspect);
						dst.x = 0;
						dst.y = (outH - dst.h) / 2;
					} else {
						dst.h = outH;
						dst.w = (int)(outH * gameAspect);
						dst.x = (outW - dst.w) / 2;
						dst.y = 0;
					}
				}
				SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
				SDL_RenderClear(s_renderer);
				SDL_FRect dstf = { (float)dst.x, (float)dst.y, (float)dst.w, (float)dst.h };
				SDL_RenderTexture(s_renderer, s_swDispTex, NULL, &dstf);
				SDL_RenderPresent(s_renderer);
			}
		}
	}

	// Frame rate limiting
	{
		static int s_fpsLimit = 60;
		static int s_refreshCounter = 0;
		s_refreshCounter++;
		if (s_refreshCounter >= 30) {
			s_refreshCounter = 0;
			s_fpsLimit = GlobalConfigManager::GetInstance()->GetValue<int>("fps_limit", 60);
		}
		auto frameElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - tickStart).count();
		int targetUs = s_fpsLimit > 0 ? (1000000 / s_fpsLimit) : 0;
		if (targetUs > 0 && frameElapsed < targetUs) {
			SDL_Delay((targetUs - (int)frameElapsed) / 1000);
		}
	}

	// FPS tracking
	g_stats.frameCount++;
	if (activeDraws > 0) g_stats.framesWithDraws++;
	g_stats.drawCallCount += activeDraws;
	auto tickEnd = std::chrono::steady_clock::now();
	g_stats.tickTotalUs += std::chrono::duration_cast<std::chrono::microseconds>(tickEnd - tickStart).count();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tickEnd - g_stats.lastLog).count();
	if (elapsed >= 1000) {
		int totalFps = g_stats.frameCount * 1000 / (elapsed ? elapsed : 1);
		int avgTickUs = g_stats.frameCount ? (int)(g_stats.tickTotalUs / g_stats.frameCount) : 0;
		__android_log_print(ANDROID_LOG_INFO, TAG, "ENGINE: %dfps %d/%d draws avg%04dus %dx%d",
			totalFps, g_stats.framesWithDraws, g_stats.frameCount, avgTickUs, g_gameW, g_gameH);
		g_stats = { std::chrono::steady_clock::now(), 0, 0, 0, 0 };
	}

	// Android native overlay (FPS/memory) — works for both Software and Vulkan modes
	_updateDebugOverlayJNI(false, activeDraws, vramSize);
	TVPUpdateCursorOverlay();
}

//------------------------------------------------------------------------------
// Coordinate conversion
//------------------------------------------------------------------------------
static void _screenToGame(float &sx, float &sy) {
	if (g_gameW <= 0 || g_gameH <= 0 || s_ScreenWidth <= 0 || s_ScreenHeight <= 0) return;
	float origX = sx, origY = sy;
	if (g_fullscreenStretch) {
		sx = sx * g_gameW / s_ScreenWidth; sy = sy * g_gameH / s_ScreenHeight;
		if (sx < 0) sx = 0; if (sx >= g_gameW) sx = g_gameW - 1;
		if (sy < 0) sy = 0; if (sy >= g_gameH) sy = g_gameH - 1;
		return;
	}
	float gameAspect = (float)g_gameW / (float)g_gameH;
	float screenAspect = (float)s_ScreenWidth / (float)s_ScreenHeight;
	int vpW, vpH, vpX, vpY;
	if (screenAspect > gameAspect) {
		vpH = s_ScreenHeight; vpW = (int)(s_ScreenHeight * gameAspect);
		vpX = (s_ScreenWidth - vpW) / 2; vpY = 0;
	} else {
		vpW = s_ScreenWidth; vpH = (int)(s_ScreenWidth / gameAspect);
		vpX = 0; vpY = (s_ScreenHeight - vpH) / 2;
	}
	float gx = (sx - vpX) * g_gameW / vpW;
	float gy = (sy - vpY) * g_gameH / vpH;
	if (gx < 0) gx = 0; if (gx >= g_gameW) gx = g_gameW - 1;
	if (gy < 0) gy = 0; if (gy >= g_gameH) gy = g_gameH - 1;
	sx = gx + g_bufferOffsetX; sy = gy; // add buffer centering offset for primaryLayer > paintBox
}

void TVPGameToScreen(float &gx, float &gy) {
	if (g_gameW <= 0 || g_gameH <= 0 || s_ScreenWidth <= 0 || s_ScreenHeight <= 0) return;
	if (g_fullscreenStretch) {
		gx = gx * s_ScreenWidth / g_gameW;
		gy = gy * s_ScreenHeight / g_gameH;
		return;
	}
	float gameAspect = (float)g_gameW / (float)g_gameH;
	float screenAspect = (float)s_ScreenWidth / (float)s_ScreenHeight;
	int vpW, vpH, vpX, vpY;
	if (screenAspect > gameAspect) {
		vpH = s_ScreenHeight; vpW = (int)(s_ScreenHeight * gameAspect);
		vpX = (s_ScreenWidth - vpW) / 2; vpY = 0;
	} else {
		vpW = s_ScreenWidth; vpH = (int)(s_ScreenWidth / gameAspect);
		vpX = 0; vpY = (s_ScreenHeight - vpH) / 2;
	}
	gx = gx * vpW / g_gameW + vpX;
	gy = gy * vpH / g_gameH + vpY;
}

//------------------------------------------------------------------------------
// Input forwarding
//------------------------------------------------------------------------------
void TVPForwardKeyEvent(int keyCode, bool isPress) {
	TVPWindowLayerSDL *win = TVPWindowLayerSDL::GetActiveWindow();
	if (!win) return;
	if (isPress) { s_Scancode[keyCode] = 0x11; win->InternalKeyDown(keyCode, TVPGetCurrentShiftKeyState()); }
	else {
		bool wasPressed = s_Scancode[keyCode] & 1;
		s_Scancode[keyCode] &= 0x10;
		if (wasPressed) win->OnKeyUp(keyCode, TVPGetCurrentShiftKeyState());
	}
}

	// Touch→mouse state for cursor mode (trackpad behavior)
static struct { bool tracking; float startX, startY; int moved; } g_touchState;

// Apply buffer centering offset for mouse events (when primaryLayer > paintBox)
static inline int _cursorX() { return g_cursorX() + g_bufferOffsetX; }
static inline int _cursorY() { return g_cursorY() + 0; }

void TVPForwardTouchBegin(int id, float x, float y) {
	TVPWindowLayerSDL *win = TVPWindowLayerSDL::GetActiveWindow();
	if (!win) return;
	if (!g_mouseMode) {
		// Touch mode: direct touch = click
		float gx = x, gy = y;
		_screenToGame(gx, gy);
		win->m_LastMouseX = (tjs_int)gx; win->m_LastMouseY = (tjs_int)gy;
		s_Scancode[VK_LBUTTON] = 0x11;
		if (win->GetWindow()) {
			TVPPostInputEvent(new tTVPOnMouseMoveInputEvent(win->GetWindow(), win->m_LastMouseX, win->m_LastMouseY, TVPGetCurrentShiftKeyState()));
			TVPPostInputEvent(new tTVPOnMouseDownInputEvent(win->GetWindow(), win->m_LastMouseX, win->m_LastMouseY, mbLeft, TVPGetCurrentShiftKeyState()));
		}
	} else {
		// Avoid resetting start on duplicate (MOUSE_BUTTON_DOWN synthesized from same touch)
		if (g_touchState.tracking) return;
		// Cursor mode (trackpad): start tracking, don't click
		g_touchState.tracking = true;
		g_touchState.startX = x; g_touchState.startY = y;
		g_touchState.moved = 0;
	}
}

void TVPForwardTouchEnd(int id, float x, float y) {
	TVPWindowLayerSDL *win = TVPWindowLayerSDL::GetActiveWindow();
	if (!win) return;
	if (!g_mouseMode) {
		float gx = x, gy = y;
		_screenToGame(gx, gy);
		win->m_LastMouseX = (tjs_int)gx; win->m_LastMouseY = (tjs_int)gy;
		s_Scancode[VK_LBUTTON] &= 0x10;
		if (win->GetWindow()) {
			TVPPostInputEvent(new tTVPOnMouseUpInputEvent(win->GetWindow(), win->m_LastMouseX, win->m_LastMouseY, mbLeft, TVPGetCurrentShiftKeyState()));
			TVPPostInputEvent(new tTVPOnClickInputEvent(win->GetWindow(), win->m_LastMouseX, win->m_LastMouseY));
		}
	} else if (g_touchState.tracking) {
		g_touchState.tracking = false;
		// If finger barely moved → tap = click at cursor position
		if (g_touchState.moved < 10) {
			win->m_LastMouseX = _cursorX(); win->m_LastMouseY = _cursorY();
			s_Scancode[VK_LBUTTON] = 0x11;
			if (win->GetWindow()) {
				TVPPostInputEvent(new tTVPOnMouseMoveInputEvent(win->GetWindow(), win->m_LastMouseX, win->m_LastMouseY, TVPGetCurrentShiftKeyState()));
				TVPPostInputEvent(new tTVPOnMouseDownInputEvent(win->GetWindow(), win->m_LastMouseX, win->m_LastMouseY, mbLeft, TVPGetCurrentShiftKeyState()));
			}
			s_Scancode[VK_LBUTTON] &= 0x10;
			if (win->GetWindow()) {
				TVPPostInputEvent(new tTVPOnMouseUpInputEvent(win->GetWindow(), win->m_LastMouseX, win->m_LastMouseY, mbLeft, TVPGetCurrentShiftKeyState()));
				TVPPostInputEvent(new tTVPOnClickInputEvent(win->GetWindow(), win->m_LastMouseX, win->m_LastMouseY));
			}
		}
	}
}

void TVPForwardTouchMove(int id, float x, float y) {
	TVPWindowLayerSDL *win = TVPWindowLayerSDL::GetActiveWindow();
	if (!win) return;
	if (!g_mouseMode) {
		float gx = x, gy = y;
		_screenToGame(gx, gy);
		win->m_LastMouseX = (tjs_int)gx; win->m_LastMouseY = (tjs_int)gy;
		if (win->GetWindow())
			TVPPostInputEvent(new tTVPOnMouseMoveInputEvent(win->GetWindow(), win->m_LastMouseX, win->m_LastMouseY, TVPGetCurrentShiftKeyState()), TVP_EPT_DISCARDABLE);
	} else if (g_touchState.tracking) {
		// Trackpad: convert absolute positions to game coords, then delta
		float prevGx = g_touchState.startX, prevGy = g_touchState.startY;
		_screenToGame(prevGx, prevGy);
		float curGx = x, curGy = y;
		_screenToGame(curGx, curGy);
		float dx = curGx - prevGx;
		float dy = curGy - prevGy;

		// Dead zone: ignore tiny jitter
		if (fabsf(dx) < 0.5f && fabsf(dy) < 0.5f) return;

		g_touchState.startX = x; g_touchState.startY = y;
		g_touchState.moved += (int)(fabsf(dx) + fabsf(dy));

		g_cursorXf = g_cursorXf + dx;
		g_cursorYf = g_cursorYf + dy;

		// Send mouse move to engine for hover effects (with buffer offset)
		win->m_LastMouseX = _cursorX(); win->m_LastMouseY = _cursorY();
		if (win->GetWindow())
			TVPPostInputEvent(new tTVPOnMouseMoveInputEvent(win->GetWindow(),
				win->m_LastMouseX, win->m_LastMouseY, TVPGetCurrentShiftKeyState()), TVP_EPT_DISCARDABLE);
	}
}

void TVPForwardTouchCancel(int id, float x, float y) { TVPForwardTouchEnd(id, x, y); }

void TVPForwardTextInput(const std::string &text) {
	TVPWindowLayerSDL *win = TVPWindowLayerSDL::GetActiveWindow();
	if (!win) return;
	for (size_t i = 0; i < text.size(); ) {
		unsigned char c = (unsigned char)text[i];
		tjs_char ch;
		if (c < 0x80) { ch = c; i += 1; }
		else if (c < 0xE0) { ch = ((c & 0x1F) << 6) | (text[i+1] & 0x3F); i += 2; }
		else { ch = ((c & 0x0F) << 12) | ((text[i+1] & 0x3F) << 6) | (text[i+2] & 0x3F); i += 3; }
		if (win->GetWindow()) TVPPostInputEvent(new tTVPOnKeyPressInputEvent(win->GetWindow(), ch));
	}
}

void SetApkPath(const std::string &p) {}

// PVR / video stubs
#include "GraphicsLoaderIntf.h"
void TVPLoadHeaderPVRv3(void*, tTJSBinaryStream*, iTJSDispatch2**) {}
iTVPTexture2D* TVPLoadPVRv3(tTJSBinaryStream*, const std::function<void(const ttstr&,const tTJSVariant&)>&) { return nullptr; }
void TVPLoadPVRv3(void*,void*,int(*)(void*,uint,uint,tTVPGraphicPixelFormat),void* (*)(void*,int),void (*)(void*,const ttstr&,const ttstr&),tTJSBinaryStream*,int,tTVPGraphicLoadMode) {}
void GetVideoOverlayObject(class tTJSNI_VideoOverlay*,struct IStream*,const tjs_char*,const tjs_char*,uint64_t,class iTVPVideoOverlay**) {}
void GetVideoLayerObject(class tTJSNI_VideoOverlay*,struct IStream*,const tjs_char*,const tjs_char*,uint64_t,class iTVPVideoOverlay**) {}
void GetMixingVideoOverlayObject(class tTJSNI_VideoOverlay*,struct IStream*,const tjs_char*,const tjs_char*,uint64_t,class iTVPVideoOverlay**) {}
void GetMFVideoOverlayObject(class tTJSNI_VideoOverlay*,struct IStream*,const tjs_char*,const tjs_char*,uint64_t,class iTVPVideoOverlay**) {}
void TVPInitLibAVCodec() {}
void TVPSetPostUpdateEvent(void(*f)()) {}
namespace TJS { void TVPConsoleLog(const char16_t*) {} }
namespace TJS { void TVPConsoleLog(const char *format, ...) {
	va_list args; va_start(args, format);
	__android_log_vprint(ANDROID_LOG_INFO, "krkr", format, args);
	va_end(args);
}}

void TVPSetScreenSizeFromSDL(int w, int h) {
	s_ScreenWidth = w; s_ScreenHeight = h;
	TVPSDLSetScreenSize(w, h);
}

// Platform stubs
bool TVPGetKeyMouseAsyncState(tjs_uint keycode, bool getcurrent) {
	if (keycode >= sizeof(s_Scancode)) return false;
	tjs_uint8 code = s_Scancode[keycode];
	s_Scancode[keycode] &= 1;
	return code & (getcurrent ? 1 : 0x10);
}
bool TVPGetJoyPadAsyncState(tjs_uint keycode, bool getcurrent) { return TVPGetKeyMouseAsyncState(keycode, getcurrent); }
ttstr TVPGetPlatformName() { return TJS_W("Android"); }
ttstr TVPGetOSName() { return TJS_W("Android"); }
void TVPOpenPatchLibUrl() {}
void TVPShowFileSelector(const std::string &, const std::string &, std::string, bool) {}
