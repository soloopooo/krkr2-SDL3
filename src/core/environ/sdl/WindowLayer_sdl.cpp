#include <SDL2/SDL.h>
#include <android/log.h>
#include <chrono>
#include <thread>
#include <string>
#include <cstdint>

bool g_fullscreenStretch = false;
#include <vector>
#include <GLES2/gl2.h>

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
#include "renderer/CCGLProgram.h"
#include "WindowIntf.h"
#include "visual/win32/DrawDevice.h"
#include "visual/LayerManager.h"
#include "visual/LayerIntf.h"

#define TAG "##krkr"

// Forward declarations
extern tjs_uint32 TVPGetCurrentShiftKeyState();
extern void TVPForceSwapBuffer();
void TVPEngineTick(); // defined later in this file
extern "C" void TVPReinitOGL();
extern "C" GLuint TVPGetFBO();
extern "C" void TVPSetRenderTarget(GLuint t);

//------------------------------------------------------------------------------
// Software renderer: fullscreen textured quad via GLES2
//------------------------------------------------------------------------------
static const char *g_vshSrc =
	"attribute vec2 aPos;\n"
	"attribute vec2 aUV;\n"
	"uniform vec2 uUVScale;\n"
	"varying vec2 vUV;\n"
	"void main() {\n"
	"  gl_Position = vec4(aPos, 0.0, 1.0);\n"
	"  vUV = aUV * uUVScale;\n"
	"}\n";

static const char *g_fshSrc =
	"precision mediump float;\n"
	"varying vec2 vUV;\n"
	"uniform sampler2D uTex;\n"
	"void main() {\n"
	"  gl_FragColor = texture2D(uTex, vUV);\n"
	"}\n";

static GLuint g_prog = 0;
static GLuint g_vbo = 0;
static GLuint g_tex = 0;
static int g_texW = 0, g_texH = 0;
static int g_scrW = 0, g_scrH = 0;
// g_LastAdapt removed — Phase 2: direct GetGLTextureName() bypasses AdapterTexture2D RefCount leak

static GLuint _compileShader(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	return s;
}

static void _initRenderer() {
	if (g_prog) return;
	g_prog = glCreateProgram();
	glAttachShader(g_prog, _compileShader(GL_VERTEX_SHADER, g_vshSrc));
	glAttachShader(g_prog, _compileShader(GL_FRAGMENT_SHADER, g_fshSrc));
	glLinkProgram(g_prog);

	// Fullscreen quad (2 triangles, 6 vertices)
	// V coordinates flipped: engine scanline 0 = top, OpenGL texture row 0 = bottom
	float verts[] = {
		-1, -1,  0, 1,
		 1, -1,  1, 1,
		-1,  1,  0, 0,
		-1,  1,  0, 0,
		 1, -1,  1, 1,
		 1,  1,  1, 0,
	};
	glGenBuffers(1, &g_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	glGenTextures(1, &g_tex);
}

static void _initTexParams() {
	glBindTexture(GL_TEXTURE_2D, g_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void _uploadTexture(int w, int h, const void *pixels) {
	const uint32_t *p = (const uint32_t*)pixels;
	__android_log_print(ANDROID_LOG_INFO, "##krkr", "GLup: w=%d h=%d tex=%u texW=%d p0=0x%08x p[72]=0x%08x",
		w, h, g_tex, g_texW, p ? p[0] : 0, (p && w > 72) ? p[72] : 0);
	glBindTexture(GL_TEXTURE_2D, g_tex);
	GLenum err = glGetError();
	if (err) __android_log_print(ANDROID_LOG_INFO, "##krkr", "GLup: bind err=0x%x", err);
	if (!g_texW) _initTexParams();
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	if (w != g_texW || h != g_texH) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		g_texW = w; g_texH = h;
	} else {
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	}
	err = glGetError();
	if (err) __android_log_print(ANDROID_LOG_INFO, "##krkr", "GLup: upload err=0x%x", err);
}

static void _renderFullscreenQuad(float uvScaleX = 1.0f, float uvScaleY = 1.0f) {
	glUseProgram(g_prog);
	glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

	GLint aPos = glGetAttribLocation(g_prog, "aPos");
	GLint aUV = glGetAttribLocation(g_prog, "aUV");
	glEnableVertexAttribArray(aPos);
	glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4*4, (void*)0);
	glEnableVertexAttribArray(aUV);
	glVertexAttribPointer(aUV, 2, GL_FLOAT, GL_FALSE, 4*4, (void*)(2*4));

	GLint uTex = glGetUniformLocation(g_prog, "uTex");
	glUniform1i(uTex, 0);
	GLint uUVScale = glGetUniformLocation(g_prog, "uUVScale");
	glUniform2f(uUVScale, uvScaleX, uvScaleY);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_tex);

	__android_log_print(ANDROID_LOG_INFO, "##krkr", "GLquad: prog=%u tex=%u vbo=%u uvScale=%.4f,%.4f aPos=%d aUV=%d uTex=%d",
		g_prog, g_tex, g_vbo, uvScaleX, uvScaleY,
		glGetAttribLocation(g_prog, "aPos"),
		glGetAttribLocation(g_prog, "aUV"),
		glGetUniformLocation(g_prog, "uTex"));
	glDrawArrays(GL_TRIANGLES, 0, 6);
	GLenum err = glGetError();
	if (err) __android_log_print(ANDROID_LOG_INFO, "##krkr", "GLquad: draw err=0x%x", err);
}

//------------------------------------------------------------------------------
// Globals (replacing MainScene.cpp statics used by engine)
//------------------------------------------------------------------------------
static tjs_uint8 s_Scancode[0x200];
static tjs_uint16 s_Keymap[0x200];
static int s_ScreenWidth = 0, s_ScreenHeight = 0;

//------------------------------------------------------------------------------
// TVPWindowLayerSDL
//------------------------------------------------------------------------------
TVPWindowLayerSDL *TVPWindowLayerSDL::s_ActiveWindow = nullptr;
TVPWindowLayerSDL *TVPWindowLayerSDL::s_LastWindow = nullptr;

TVPWindowLayerSDL::TVPWindowLayerSDL(tTJSNI_Window *w)
	: m_Window(w)
	, m_Visible(false)
	, m_Width(0), m_Height(0)
	, m_LastMouseX(0), m_LastMouseY(0)
	, m_InModal(false)
	, m_ModalResult(0)
	, m_ZoomNumer(1), m_ZoomDenom(1)
	, m_UseMouseKey(false)
	, m_MouseLeftEmulated(false), m_MouseRightEmulated(false)
	, m_LastMouseKeyTick(0)
	, m_MouseKeyXAccel(0), m_MouseKeyYAccel(0)
	, m_Prev(nullptr), m_Next(nullptr)
{
	m_LastMouseKeyTick = TVPGetRoughTickCount32();
	m_Prev = s_LastWindow;
	s_LastWindow = this;
	if (m_Prev) m_Prev->m_Next = this;
}

TVPWindowLayerSDL::~TVPWindowLayerSDL() {
	if (s_LastWindow == this) s_LastWindow = m_Prev;
	if (m_Next) m_Next->m_Prev = m_Prev;
	if (m_Prev) m_Prev->m_Next = m_Next;
	if (s_ActiveWindow == this) {
		s_ActiveWindow = m_Prev ? m_Prev : m_Next;
	}
}

void TVPWindowLayerSDL::SetPaintBoxSize(tjs_int w, tjs_int h) {
	m_Width = w; m_Height = h;
}

bool TVPWindowLayerSDL::GetFormEnabled() { return m_Visible; }
void TVPWindowLayerSDL::SetDefaultMouseCursor() {}

void TVPWindowLayerSDL::GetCursorPos(tjs_int &x, tjs_int &y) {
	x = m_LastMouseX; y = m_LastMouseY;
}

void TVPWindowLayerSDL::SetCursorPos(tjs_int x, tjs_int y) {
	m_LastMouseX = x; m_LastMouseY = y;
}

void TVPWindowLayerSDL::SetHintText(const ttstr &) {}

void TVPWindowLayerSDL::SetAttentionPoint(tjs_int, tjs_int,
	const struct tTVPFont *) {}

void TVPWindowLayerSDL::ZoomRectangle(tjs_int &left, tjs_int &top,
	tjs_int &right, tjs_int &bottom) {
	left = (tjs_int64)left * m_ZoomNumer / m_ZoomDenom;
	top = (tjs_int64)top * m_ZoomNumer / m_ZoomDenom;
	right = (tjs_int64)right * m_ZoomNumer / m_ZoomDenom;
	bottom = (tjs_int64)bottom * m_ZoomNumer / m_ZoomDenom;
}

void TVPWindowLayerSDL::BringToFront() {
	s_ActiveWindow = this;
}

void TVPWindowLayerSDL::ShowWindowAsModal() {
	m_InModal = true;
	m_Visible = true;
	BringToFront();
	m_ModalResult = 0;
	while (this == s_ActiveWindow && !m_ModalResult) {
		TVPEngineTick();
		if (::Application->IsTarminate()) {
			m_ModalResult = 1;
		}
		if (m_ModalResult != 0) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	m_InModal = false;
}

bool TVPWindowLayerSDL::GetVisible() { return m_Visible; }

void TVPWindowLayerSDL::SetVisible(bool bVisible) {
	m_Visible = bVisible;
	if (bVisible) BringToFront();
}

const char *TVPWindowLayerSDL::GetCaption() { return m_Caption.c_str(); }
void TVPWindowLayerSDL::SetCaption(const std::string &s) { m_Caption = s; }

void TVPWindowLayerSDL::SetWidth(tjs_int w) { m_Width = w; }
void TVPWindowLayerSDL::SetHeight(tjs_int h) { m_Height = h; }
void TVPWindowLayerSDL::SetSize(tjs_int w, tjs_int h) { m_Width = w; m_Height = h; }
void TVPWindowLayerSDL::GetSize(tjs_int &w, tjs_int &h) { w = m_Width; h = m_Height; }
tjs_int TVPWindowLayerSDL::GetWidth() const { return m_Width; }
tjs_int TVPWindowLayerSDL::GetHeight() const { return m_Height; }
void TVPWindowLayerSDL::GetWinSize(tjs_int &w, tjs_int &h) { w = s_ScreenWidth; h = s_ScreenHeight; }

void TVPWindowLayerSDL::SetZoom(tjs_int numer, tjs_int denom) {
	m_ZoomNumer = numer; m_ZoomDenom = denom;
}

void TVPWindowLayerSDL::UpdateDrawBuffer(iTVPTexture2D *) {
	// FIXME: render texture to SDL surface / GL
}

void TVPWindowLayerSDL::InvalidateClose() {
	if (m_Window) {
		iTJSDispatch2 *obj = m_Window->GetOwnerNoAddRef();
		obj->Invalidate(0, nullptr, nullptr, obj);
		m_Window = nullptr;
	}
	delete this;
}

bool TVPWindowLayerSDL::GetWindowActive() { return s_ActiveWindow == this; }

void TVPWindowLayerSDL::Close() {
	__android_log_print(ANDROID_LOG_INFO, "##krkr",
		"TVPWindowLayerSDL::Close m_InModal=%d m_Window=%p", m_InModal, m_Window);
	if (m_InModal) {
		m_ModalResult = 1;
		return;
	}
	if (!m_Window) return;

	// Fire onCloseQuery synchronously; TJS handler may show confirmation dialog
	iTJSDispatch2 *obj = m_Window->GetOwnerNoAddRef();
	if (obj) {
		__android_log_print(ANDROID_LOG_INFO, "##krkr",
			"TVPWindowLayerSDL::Close firing onCloseQuery");
		tTJSVariant arg[1] = { true };
		static ttstr eventname(TJS_W("onCloseQuery"));
		TVPPostEvent(obj, obj, eventname, 0, TVP_EPT_IMMEDIATE, 1, arg);
		// Event processed synchronously above; OnCloseQueryCalled was invoked
		// If confirmed, m_CloseConfirmed is set and cleanup happens there
	}
}

void TVPWindowLayerSDL::OnCloseQueryCalled(bool b) {
	__android_log_print(ANDROID_LOG_INFO, "##krkr",
		"TVPWindowLayerSDL::OnCloseQueryCalled b=%d m_Window=%p", b, m_Window);
	if (!b || !m_Window) {
		__android_log_print(ANDROID_LOG_INFO, "##krkr",
			"  close cancelled or no window");
		return;
	}
	iTJSDispatch2 *obj = m_Window->GetOwnerNoAddRef();
	if (obj) {
		__android_log_print(ANDROID_LOG_INFO, "##krkr",
			"  invalidating owner %p", obj);
		obj->Invalidate(0, nullptr, nullptr, obj);
	}
	m_Window = nullptr;
	s_ActiveWindow = nullptr;
}

void TVPWindowLayerSDL::InternalKeyDown(tjs_uint16 key, tjs_uint32 shift) {
	if (m_Window) {
		tjs_uint32 tick = TVPGetRoughTickCount32();
		TVPPushEnvironNoise(&tick, sizeof(tick));
		TVPPushEnvironNoise(&key, sizeof(key));
		TVPPushEnvironNoise(&shift, sizeof(shift));

		if (m_UseMouseKey) {
			if (key == VK_RETURN || key == VK_SPACE || key == VK_ESCAPE ||
				key == VK_PAD1 || key == VK_PAD2) {
				if (m_LastMouseX >= 0 && m_LastMouseY >= 0 &&
					m_LastMouseX < m_Width && m_LastMouseY < m_Height) {
					// emulated mouse click
					if (key == VK_RETURN || key == VK_SPACE || key == VK_PAD1) {
						m_MouseLeftEmulated = true;
					}
					if (key == VK_ESCAPE || key == VK_PAD2) {
						m_MouseRightEmulated = true;
					}
				}
				return;
			}
		}
		TVPPostInputEvent(new tTVPOnKeyDownInputEvent(m_Window, key, shift));
	}
}

void TVPWindowLayerSDL::OnKeyUp(tjs_uint16 vk, int shift) {
	if (m_Window) {
		TVPPostInputEvent(new tTVPOnKeyUpInputEvent(m_Window, vk, shift));
	}
}

void TVPWindowLayerSDL::OnKeyPress(tjs_uint16 vk, int, bool, bool) {
	if (m_Window && vk) {
		TVPPostInputEvent(new tTVPOnKeyPressInputEvent(m_Window, vk));
	}
}

tTVPImeMode TVPWindowLayerSDL::GetDefaultImeMode() const { return imDisable; }
void TVPWindowLayerSDL::SetImeMode(tTVPImeMode) {}
void TVPWindowLayerSDL::ResetImeMode() {}

void TVPWindowLayerSDL::UpdateWindow(tTVPUpdateType) {
	if (m_Window) {
		tTVPRect r = {0, 0, m_Width, m_Height};
		m_Window->NotifyWindowExposureToLayer(r);
		TVPDeliverWindowUpdateEvents();
	}
}

void TVPWindowLayerSDL::SetVisibleFromScript(bool b) { SetVisible(b); }
void TVPWindowLayerSDL::SetUseMouseKey(bool b) { m_UseMouseKey = b; }
bool TVPWindowLayerSDL::GetUseMouseKey() const { return m_UseMouseKey; }
void TVPWindowLayerSDL::ResetMouseVelocity() {}
void TVPWindowLayerSDL::ResetTouchVelocity(tjs_int) {}
bool TVPWindowLayerSDL::GetMouseVelocity(float &, float &, float &) const { return false; }

void TVPWindowLayerSDL::TickBeat() {
	if (m_UseMouseKey && s_ActiveWindow == this) {
		GenerateMouseEvent(false, false, false, false);
	}
}

void TVPWindowLayerSDL::GenerateMouseEvent(bool fl, bool fr, bool fu, bool fd) {
	if (!fl && !fr && !fu && !fd) {
		if (TVPGetRoughTickCount32() - 45 < m_LastMouseKeyTick) return;
	}
	bool left = fl || (s_Scancode[VK_LEFT] & 1);
	bool right = fr || (s_Scancode[VK_RIGHT] & 1);
	bool up = fu || (s_Scancode[VK_UP] & 1);
	bool down = fd || (s_Scancode[VK_DOWN] & 1);

	if (!right && !left && !up && !down) {
		m_MouseKeyXAccel = m_MouseKeyYAccel = 0;
	}

	if (left) if (m_MouseKeyXAccel > -30) m_MouseKeyXAccel -= 2;
	if (right) if (m_MouseKeyXAccel < 30) m_MouseKeyXAccel += 2;
	if (up) if (m_MouseKeyYAccel > -30) m_MouseKeyYAccel -= 2;
	if (down) if (m_MouseKeyYAccel < 30) m_MouseKeyYAccel += 2;

	m_LastMouseX += m_MouseKeyXAccel >> 1;
	m_LastMouseY += m_MouseKeyYAccel >> 1;
	m_LastMouseKeyTick = TVPGetRoughTickCount32();
}

//------------------------------------------------------------------------------
// Global functions
//------------------------------------------------------------------------------

iWindowLayer *TVPCreateAndAddWindow(tTJSNI_Window *w) {
	__android_log_print(ANDROID_LOG_INFO, TAG, "TVPCreateAndAddWindow (SDL)");
	auto *win = new TVPWindowLayerSDL(w);
	// Set default size to screen dimensions
	if (s_ScreenWidth > 0 && s_ScreenHeight > 0)
		win->SetPaintBoxSize(s_ScreenWidth, s_ScreenHeight);
	return win;
}

void TVPRemoveWindowLayer(iWindowLayer *lay) {
	__android_log_print(ANDROID_LOG_INFO, TAG, "TVPRemoveWindowLayer (SDL)");
	delete static_cast<TVPWindowLayerSDL *>(lay);
}

tTJSNI_Window *TVPGetActiveWindow() {
	auto *w = TVPWindowLayerSDL::GetActiveWindow();
	return w ? w->GetWindow() : nullptr;
}

bool TVPGetScreenSize(tjs_int idx, tjs_int &w, tjs_int &h) {
	if (idx != 0) return false;
	w = s_ScreenWidth;
	h = s_ScreenHeight;
	return true;
}

ttstr TVPGetDataPath() {
	// Returns the app's writable data path
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
	std::string utf8 = l.AsNarrowStdString();
	__android_log_print(ANDROID_LOG_INFO, TAG, "%s", utf8.c_str());
}

//------------------------------------------------------------------------------
// Per-frame engine tick
//------------------------------------------------------------------------------
#include <EGL/egl.h>

static bool s_glInited = false;
void ResetGLState() {
	s_glInited = false;
	g_prog = 0;
	g_vbo = 0;
	g_tex = 0;
	g_texW = g_texH = 0;
}
static void InitGL() {
	if (s_glInited) return;
	EGLDisplay dpy = eglGetCurrentDisplay();
	EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
	__android_log_print(ANDROID_LOG_INFO, "##krkr", "InitGL: dpy=%p surf=%p",
		dpy, surf);
	if (dpy == EGL_NO_DISPLAY || surf == EGL_NO_SURFACE) return;
	glClearColor(0.0f, 0.3f, 0.6f, 1.0f);
	_initRenderer();
	__android_log_print(ANDROID_LOG_INFO, "##krkr", "InitGL: calling TVPReinitOGL");
	TVPReinitOGL();
	s_glInited = true;
}

static struct { int w, h; std::vector<uint32_t> pix; } g_frameBuf;
// Game logical resolution (set each frame from DrawBuffer size)
static int g_gameW = 0, g_gameH = 0;

// Engine-side FPS tracking (lightweight — no per-pixel scanning)
static struct {
	std::chrono::steady_clock::time_point lastLog;
	int frameCount;
	int framesWithDraws;  // frames where drawCount > 0 (engine was active)
	int drawCallCount;
	long long tickTotalUs; // cumulative TVPEngineTick duration
} g_stats = { std::chrono::steady_clock::now(), 0, 0, 0, 0 };

// Use OGL renderer's GPU texture directly — bypass AdapterTexture2D to avoid RefCount leak
static bool _useOGLTexture(iTVPBaseBitmap *drawBuf) {
	if (!drawBuf || TVPIsSoftwareRenderManager()) return false;
	iTVPTexture2D *tex = drawBuf->GetTexture();
	if (!tex) return false;
	GLuint glName = tex->GetGLTextureName();
	if (!glName) return false;
	g_tex = glName;
	g_texW = tex->GetWidth();
	g_texH = tex->GetHeight();
	glBindTexture(GL_TEXTURE_2D, g_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return true;
}

void TVPEngineTick() {
	auto tickStart = std::chrono::steady_clock::now();

	DrainAndroidEventQueue();
	TVPProcessSDLEvents();
	::Application->Run();
	iTVPTexture2D::RecycleProcess();

	// Ensure GL context init (FBO, shaders) BEFORE compositing
	InitGL();
	if (!s_glInited) return;

	// Reset FBO tracking before compositing so OGL renderer's
	// TVPSetRenderTarget calls properly save/restore FBO state.
	TVPSetRenderTarget(0);
	// Force window update compositing (child layers render onto DrawBuffer)
	TVPDeliverWindowUpdateEvents();
	// Ensure OGL compositing commands are complete
	glFinish();
	TVPSetRenderTarget(0);
	// Post-update: restore GL state (blend, pixel store, viewport) after compositing
	if (_postUpdate) _postUpdate();
	// Check for GL errors from compositing
	{
		GLenum e; int n = 0;
		while ((e = glGetError()) != GL_NO_ERROR)
			__android_log_print(ANDROID_LOG_INFO, "##krkr", "RENDER: compositeErr #%d=0x%x", n++, e);
	}
	// Read composited pixels from LayerManager::DrawBuffer
	__android_log_print(ANDROID_LOG_INFO, "##krkr", "RENDER: prog=%u tex=%u vbo=%u texWH=%dx%d isGPU=%d",
		g_prog, g_tex, g_vbo, g_texW, g_texH, TVPIsSoftwareRenderManager() ? 0 : 1);
	glDisable(GL_BLEND);
	if ((g_tex && !glIsTexture(g_tex)) || (g_prog && !glIsProgram(g_prog))) {
		__android_log_print(ANDROID_LOG_INFO, "##krkr", "RENDER: GL objects corrupt, reinit");
		glDeleteTextures(1, &g_tex); g_tex = 0;
		glDeleteProgram(g_prog); g_prog = 0;
		glDeleteBuffers(1, &g_vbo); g_vbo = 0;
		g_texW = g_texH = 0;
	}
	if (!g_prog) _initRenderer();
	GLenum err = glGetError();
	if (err) __android_log_print(ANDROID_LOG_INFO, "##krkr", "RENDER: glErr before=0x%x", err);
	bool rendered = false;
	int activeDraws = 0;
	g_gameW = 0; g_gameH = 0;
	float uvScaleX = 1.0f, uvScaleY = 1.0f;
	{
	TVPWindowLayerSDL *sdlWin = TVPWindowLayerSDL::GetActiveWindow();
	__android_log_print(ANDROID_LOG_INFO, "##krkr", "RENDER: sdlWin=%p", sdlWin);
	if (sdlWin) {
		tTJSNI_Window *tjsWin = sdlWin->GetWindow();
		iTVPDrawDevice *dd = tjsWin ? tjsWin->GetDrawDevice() : nullptr;
		iTVPBaseBitmap *drawBuf = nullptr;
		__android_log_print(ANDROID_LOG_INFO, "##krkr", "RENDER: tjsWin=%p dd=%p", tjsWin, dd);
		if (dd) {
			unsigned int dc = 0; uint64_t vm = 0;
			TVPGetRenderManager()->GetRenderStat(dc, vm);
			activeDraws = (int)dc;
			auto *ddc = static_cast<tTVPDrawDevice*>(dd);
			for (size_t i = 0; ; i++) {
				iTVPLayerManager *lm = ddc->GetLayerManagerAt(i);
				if (!lm) break;
				iTVPBaseBitmap *buf = lm->GetDrawBuffer();
				__android_log_print(ANDROID_LOG_INFO, "##krkr", "RENDER: lm[%zu]=%p buf=%p", i, lm, buf);
				if (buf) drawBuf = buf;
			}
		}
		__android_log_print(ANDROID_LOG_INFO, "##krkr", "RENDER: drawBuf=%p w=%d", drawBuf, drawBuf ? (int)drawBuf->GetWidth() : -1);
		if (drawBuf) {
			g_gameW = (int)drawBuf->GetWidth();
			g_gameH = (int)drawBuf->GetHeight();
			int w = g_gameW;
			int h = g_gameH;
			// Use GPU texture directly if OGL renderer is active
			if (_useOGLTexture(drawBuf)) {
				uvScaleX = (float)g_gameW / (float)g_texW;
				uvScaleY = (float)g_gameH / (float)g_texH;
				__android_log_print(ANDROID_LOG_INFO, "##krkr", "RENDER: OGL tex=%u %dx%d uvScale=%.4f,%.4f",
					g_tex, g_texW, g_texH, uvScaleX, uvScaleY);
				// Read pixel from the ACTUAL DrawBuffer texture to verify compositing
				{
					uint32_t dpix = 0xDEAD;
					TVPSetRenderTarget(g_tex);
					GLenum fbSt = glCheckFramebufferStatus(GL_FRAMEBUFFER);
					glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &dpix);
					GLenum derr = glGetError();
					__android_log_print(ANDROID_LOG_INFO, "##krkr", "DBUF: g_tex=%u dpix=0x%08x err=0x%x fbSt=0x%x",
						g_tex, dpix, derr, fbSt);
					TVPSetRenderTarget(0);
				}
				// Check primary layer's MainImage — use GetGLTextureName to avoid AdapterTexture2D
				{
					TVPWindowLayerSDL *sw = TVPWindowLayerSDL::GetActiveWindow();
					if (sw) {
						tTJSNI_Window *tw = sw->GetWindow();
						if (tw) {
							iTVPDrawDevice *dd = tw->GetDrawDevice();
							if (dd) {
								auto *ddc = static_cast<tTVPDrawDevice*>(dd);
								iTVPLayerManager *lm = ddc->GetLayerManagerAt(0);
								if (lm) {
									tTJSNI_BaseLayer *pri = lm->GetPrimaryLayer();
									if (pri && pri->MainImage) {
										iTVPTexture2D *mainTex = pri->MainImage->GetTexture();
										if (mainTex) {
											unsigned int glName = mainTex->GetGLTextureName();
											if (glName) {
												uint32_t spix = 0xDEAD;
												TVPSetRenderTarget(glName);
												glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &spix);
												GLenum merr = glGetError();
												__android_log_print(ANDROID_LOG_INFO, "##krkr", "MAINIMG: tex=%u spix=0x%08x err=0x%x",
													glName, spix, merr);
												TVPSetRenderTarget(0);
											}
										}
									}
								}
							}
						}
					}
				}
				rendered = true;
			} else {
				// Software path: read pixels and upload
				int pitched = (int)drawBuf->GetPitchBytes();
				const void *pxbuf = drawBuf->GetScanLine(0);
				if (pxbuf && w > 0 && h > 0) {
					if (w != g_frameBuf.w || h != g_frameBuf.h) {
						g_frameBuf.w = w; g_frameBuf.h = h;
						g_frameBuf.pix.resize(w * h, 0);
					}
					auto *dst = g_frameBuf.pix.data();
					if (pitched == w * 4) {
						memcpy(dst, pxbuf, h * w * 4);
					} else {
						for (int y = 0; y < h; y++) {
							memcpy((uint8_t*)dst + y * w * 4, (const uint8_t*)pxbuf + pitched * y, w * 4);
						}
					}
					// Diagnostic: sample a few DrawBuffer pixels
					{
						uint32_t *p = (uint32_t*)dst;
						uint32_t pc = p[h/2 * w + w/2];    // center
						uint32_t p0 = p[0];                  // top-left
						__android_log_print(ANDROID_LOG_INFO, "##krkr", "SWDBUF: (0,0)=0x%08x (%d,%d)=0x%08x",
							p0, w/2, h/2, pc);
					}
					_uploadTexture(w, h, g_frameBuf.pix.data());
					rendered = true;
				}
			}
		}
	}
	}
	if (!rendered) {
		uvScaleX = uvScaleY = 1.0f;
		g_gameW = s_ScreenWidth ? s_ScreenWidth : 1280;
		g_gameH = s_ScreenHeight ? s_ScreenHeight : 720;
		int w = g_gameW, h = g_gameH;
		if (w != g_frameBuf.w || h != g_frameBuf.h) {
			g_frameBuf.w = w; g_frameBuf.h = h;
			g_frameBuf.pix.resize(w * h, 0);
		}
		auto *p = g_frameBuf.pix.data();
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				uint8_t r = (uint8_t)((float)(x) * 255.0f / (float)w);
				uint8_t g = (uint8_t)((float)(y) * 255.0f / (float)h);
				*p++ = 0xFF000000 | (128 << 16) | (g << 8) | r;
			}
		}
		_uploadTexture(w, h, g_frameBuf.pix.data());
	}
	glViewport(0, 0, s_ScreenWidth, s_ScreenHeight);
	glClear(GL_COLOR_BUFFER_BIT);
	if (g_gameW > 0 && g_gameH > 0 && s_ScreenWidth > 0 && s_ScreenHeight > 0) {
		if (g_fullscreenStretch) {
			glViewport(0, 0, s_ScreenWidth, s_ScreenHeight);
		} else {
			float gameAspect = (float)g_gameW / (float)g_gameH;
			float screenAspect = (float)s_ScreenWidth / (float)s_ScreenHeight;
			int vpW, vpH, vpX, vpY;
			if (screenAspect > gameAspect) {
				vpH = s_ScreenHeight;
				vpW = (int)(s_ScreenHeight * gameAspect);
				vpX = (s_ScreenWidth - vpW) / 2;
				vpY = 0;
			} else {
				vpW = s_ScreenWidth;
				vpH = (int)(s_ScreenWidth / gameAspect);
				vpX = 0;
				vpY = (s_ScreenHeight - vpH) / 2;
			}
			glViewport(vpX, vpY, vpW, vpH);
		}
	}
	_renderFullscreenQuad(uvScaleX, uvScaleY);

	// FPS tracking (lightweight)
	g_stats.frameCount++;
	if (activeDraws > 0) g_stats.framesWithDraws++;
	g_stats.drawCallCount += activeDraws;
	auto tickEnd = std::chrono::steady_clock::now();
	g_stats.tickTotalUs += std::chrono::duration_cast<std::chrono::microseconds>(tickEnd - tickStart).count();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tickEnd - g_stats.lastLog).count();
	if (elapsed >= 1000) {
		int totalFps = g_stats.frameCount * 1000 / (elapsed ? elapsed : 1);
		int avgTickUs = g_stats.frameCount ? (int)(g_stats.tickTotalUs / g_stats.frameCount) : 0;
		__android_log_print(ANDROID_LOG_INFO, TAG,
			"ENGINE: %dfps %d/%d draws avg%04dus %dx%d",
			totalFps, g_stats.framesWithDraws, g_stats.frameCount,
			avgTickUs, g_gameW, g_gameH);
		g_stats.frameCount = 0;
		g_stats.framesWithDraws = 0;
		g_stats.drawCallCount = 0;
		g_stats.tickTotalUs = 0;
		g_stats.lastLog = tickEnd;
	}
}

//------------------------------------------------------------------------------
// Coordinate conversion: screen space → game (logical) space
// The game renders at g_gameW × g_gameH with aspect-ratio viewport.
// Screen coords: (sx, sy) with y-down (Android touch origin at top-left).
// Game coords: (gx, gy) with y-down (Kirikiri engine origin at top-left).
// Both use same orientation, only need to account for letterbox offset + scale.
//------------------------------------------------------------------------------
static void _screenToGame(float &sx, float &sy) {
	if (g_gameW <= 0 || g_gameH <= 0 || s_ScreenWidth <= 0 || s_ScreenHeight <= 0) return;
	if (g_fullscreenStretch) {
		float gx = sx * g_gameW / s_ScreenWidth;
		float gy = sy * g_gameH / s_ScreenHeight;
		if (gx < 0) gx = 0; if (gx >= g_gameW) gx = g_gameW - 1;
		if (gy < 0) gy = 0; if (gy >= g_gameH) gy = g_gameH - 1;
		sx = gx;
		sy = gy;
		return;
	}
	float gameAspect = (float)g_gameW / (float)g_gameH;
	float screenAspect = (float)s_ScreenWidth / (float)s_ScreenHeight;
	int vpW, vpH, vpX, vpY;
	if (screenAspect > gameAspect) {
		vpH = s_ScreenHeight;
		vpW = (int)(s_ScreenHeight * gameAspect);
		vpX = (s_ScreenWidth - vpW) / 2;
		vpY = 0;
	} else {
		vpW = s_ScreenWidth;
		vpH = (int)(s_ScreenWidth / gameAspect);
		vpX = 0;
		vpY = (s_ScreenHeight - vpH) / 2;
	}
	// Subtract viewport offset, scale to game coords
	float gx = (sx - vpX) * g_gameW / vpW;
	float gy = (sy - vpY) * g_gameH / vpH;
	// Clamp to valid game area
	if (gx < 0) gx = 0; if (gx >= g_gameW) gx = g_gameW - 1;
	if (gy < 0) gy = 0; if (gy >= g_gameH) gy = g_gameH - 1;
	sx = gx;
	sy = gy;
}
//------------------------------------------------------------------------------
// Input forwarding helpers (called from JNI)
//------------------------------------------------------------------------------
void TVPForwardKeyEvent(int keyCode, bool isPress) {
	TVPWindowLayerSDL *win = TVPWindowLayerSDL::GetActiveWindow();
	if (!win) return;

	if (isPress) {
		s_Scancode[keyCode] = 0x11;
		win->InternalKeyDown(keyCode, TVPGetCurrentShiftKeyState());
	} else {
		bool wasPressed = s_Scancode[keyCode] & 1;
		s_Scancode[keyCode] &= 0x10;
		if (wasPressed) {
			win->OnKeyUp(keyCode, TVPGetCurrentShiftKeyState());
		}
	}
}

void TVPForwardTouchBegin(int id, float x, float y) {
	TVPWindowLayerSDL *win = TVPWindowLayerSDL::GetActiveWindow();
	if (!win) return;

	_screenToGame(x, y);
	win->m_LastMouseX = (tjs_int)x;
	win->m_LastMouseY = (tjs_int)y;
	s_Scancode[VK_LBUTTON] = 0x11;

	if (win->GetWindow()) {
		TVPPostInputEvent(new tTVPOnMouseMoveInputEvent(win->GetWindow(),
			win->m_LastMouseX, win->m_LastMouseY, TVPGetCurrentShiftKeyState()));
		TVPPostInputEvent(new tTVPOnMouseDownInputEvent(win->GetWindow(),
			win->m_LastMouseX, win->m_LastMouseY, mbLeft,
			TVPGetCurrentShiftKeyState()));
	}
}

void TVPForwardTouchEnd(int id, float x, float y) {
	TVPWindowLayerSDL *win = TVPWindowLayerSDL::GetActiveWindow();
	if (!win) return;

	_screenToGame(x, y);
	win->m_LastMouseX = (tjs_int)x;
	win->m_LastMouseY = (tjs_int)y;
	s_Scancode[VK_LBUTTON] &= 0x10;

	if (win->GetWindow()) {
		TVPPostInputEvent(new tTVPOnMouseUpInputEvent(win->GetWindow(),
			win->m_LastMouseX, win->m_LastMouseY, mbLeft,
			TVPGetCurrentShiftKeyState()));
	}
}

void TVPForwardTouchMove(int id, float x, float y) {
	TVPWindowLayerSDL *win = TVPWindowLayerSDL::GetActiveWindow();
	if (!win) return;

	_screenToGame(x, y);
	win->m_LastMouseX = (tjs_int)x;
	win->m_LastMouseY = (tjs_int)y;
	if (win->GetWindow()) {
		TVPPostInputEvent(new tTVPOnMouseMoveInputEvent(win->GetWindow(),
			win->m_LastMouseX, win->m_LastMouseY, TVPGetCurrentShiftKeyState()),
			TVP_EPT_DISCARDABLE);
	}
}

void TVPForwardTouchCancel(int id, float x, float y) {
	TVPForwardTouchEnd(id, x, y);
}

void TVPForwardCharInput(int keyCode) {
	TVPWindowLayerSDL *win = TVPWindowLayerSDL::GetActiveWindow();
	if (!win) return;
	if (win->GetWindow()) {
		TVPPostInputEvent(new tTVPOnKeyPressInputEvent(win->GetWindow(), (tjs_char)keyCode));
	}
}

void TVPForwardTextInput(const std::string &text) {
	TVPWindowLayerSDL *win = TVPWindowLayerSDL::GetActiveWindow();
	if (!win) return;
	// Convert UTF-8 string to TJS char string
	for (size_t i = 0; i < text.size(); ) {
		unsigned char c = (unsigned char)text[i];
		tjs_char ch;
		if (c < 0x80) {
			ch = c; i += 1;
		} else if (c < 0xE0) {
			ch = ((c & 0x1F) << 6) | (text[i+1] & 0x3F);
			i += 2;
		} else {
			ch = ((c & 0x0F) << 12) | ((text[i+1] & 0x3F) << 6) | (text[i+2] & 0x3F);
			i += 3;
		}
		if (win->GetWindow()) {
			TVPPostInputEvent(new tTVPOnKeyPressInputEvent(win->GetWindow(), ch));
		}
	}
}

// SetApkPath — called from JNI bridge to cache APK path for cocos2d stubs
static std::string s_apkPath;
void SetApkPath(const std::string &p) { s_apkPath = p; }

// PVR texture stubs (LoadPVRv3.cpp excluded)
#include "GraphicsLoaderIntf.h"  // for tTVPGraphicLoadMode etc
void TVPLoadHeaderPVRv3(void* format, tTJSBinaryStream* stream, iTJSDispatch2** list) {}
iTVPTexture2D* TVPLoadPVRv3(tTJSBinaryStream* stream, const std::function<void(const ttstr&, const tTJSVariant&)>&) { return nullptr; }
void TVPLoadPVRv3(void* buf, void* callback, int (*read)(void*,uint,uint,tTVPGraphicPixelFormat),
	void* (*alloc)(void*,int), void (*msg)(void*,const ttstr&,const ttstr&),
	tTJSBinaryStream* stream, int, tTVPGraphicLoadMode) {}

// Video overlay stubs (KRMoviePlayer/KRMovieLayer excluded)
class tTJSNI_VideoOverlay;
class iTVPVideoOverlay;
struct IStream;
void GetVideoOverlayObject(tTJSNI_VideoOverlay*, IStream*, const tjs_char*, const tjs_char*, uint64_t, iTVPVideoOverlay**) {}
void GetVideoLayerObject(tTJSNI_VideoOverlay*, IStream*, const tjs_char*, const tjs_char*, uint64_t, iTVPVideoOverlay**) {}
void GetMixingVideoOverlayObject(tTJSNI_VideoOverlay*, IStream*, const tjs_char*, const tjs_char*, uint64_t, iTVPVideoOverlay**) {}
void GetMFVideoOverlayObject(tTJSNI_VideoOverlay*, IStream*, const tjs_char*, const tjs_char*, uint64_t, iTVPVideoOverlay**) {}
void TVPInitLibAVCodec() {}
namespace TJS { void TVPConsoleLog(const char16_t*) {} }
namespace TJS { void TVPConsoleLog(const char *format, ...) {
	va_list args; va_start(args, format);
	__android_log_vprint(ANDROID_LOG_INFO, "krkr", format, args);
	va_end(args);
}}
static void(*_postUpdate)() = nullptr;
void TVPSetPostUpdateEvent(void(*f)()) { _postUpdate = f; }
const char* cocos2d::GLProgram::SHADER_NAME_POSITION_TEXTURE = "ShaderPositionTexture";



void TVPSetScreenSizeFromSDL(int w, int h) {
	s_ScreenWidth = w;
	s_ScreenHeight = h;
	// EGL context might have been recreated — mark GL state for re-init
	ResetGLState();
}

//------------------------------------------------------------------------------
// Platform stubs (replacing MainScene.cpp exports)
//------------------------------------------------------------------------------
bool TVPGetKeyMouseAsyncState(tjs_uint keycode, bool getcurrent) {
	if (keycode >= sizeof(s_Scancode)) return false;
	tjs_uint8 code = s_Scancode[keycode];
	s_Scancode[keycode] &= 1;
	return code & (getcurrent ? 1 : 0x10);
}

bool TVPGetJoyPadAsyncState(tjs_uint keycode, bool getcurrent) {
	return TVPGetKeyMouseAsyncState(keycode, getcurrent);
}

ttstr TVPGetPlatformName() { return TJS_W("Android"); }
ttstr TVPGetOSName() { return TJS_W("Android"); }
void TVPOpenPatchLibUrl() {} // stub: no URL opening in SDL variant
void TVPShowFileSelector(const std::string &, const std::string &, std::string, bool) {} // stub
