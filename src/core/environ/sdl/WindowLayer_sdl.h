#pragma once

#include <string>

#include "tjsCommHead.h"
#include "WindowIntf.h"   // iWindowLayer
#include "TVPWindow.h"    // iWindowLayer
#include "SDL3/SDL.h"

class tTJSNI_Window;
class iTVPTexture2D;

//------------------------------------------------------------------------------
// SDL2 window layer — minimal iWindowLayer implementation
// replaces TVPWindowLayer from MainScene.cpp (cocos2d)
//------------------------------------------------------------------------------
class TVPWindowLayerSDL : public iWindowLayer {
public:
	TVPWindowLayerSDL(tTJSNI_Window *w);
	virtual ~TVPWindowLayerSDL();

	void SetPaintBoxSize(tjs_int w, tjs_int h) override;
	bool GetFormEnabled() override;
	void SetDefaultMouseCursor() override;
	void GetCursorPos(tjs_int &x, tjs_int &y) override;
	void SetCursorPos(tjs_int x, tjs_int y) override;
	void SetHintText(const ttstr &text) override;
	void SetAttentionPoint(tjs_int left, tjs_int top,
		const struct tTVPFont *font) override;
	void ZoomRectangle(tjs_int &left, tjs_int &top,
		tjs_int &right, tjs_int &bottom) override;
	void BringToFront() override;
	void ShowWindowAsModal() override;
	bool GetVisible() override;
	void SetVisible(bool bVisible) override;
	const char *GetCaption() override;
	void SetCaption(const std::string &s) override;
	void SetWidth(tjs_int w) override;
	void SetHeight(tjs_int h) override;
	void SetSize(tjs_int w, tjs_int h) override;
	void GetSize(tjs_int &w, tjs_int &h) override;
	tjs_int GetWidth() const override;
	tjs_int GetHeight() const override;
	void GetWinSize(tjs_int &w, tjs_int &h) override;
	void SetZoom(tjs_int numer, tjs_int denom) override;
	void UpdateDrawBuffer(iTVPTexture2D *tex) override;
	void InvalidateClose() override;
	bool GetWindowActive() override;
	void Close() override;
	void OnCloseQueryCalled(bool b) override;
	void InternalKeyDown(tjs_uint16 key, tjs_uint32 shift) override;
	void OnKeyUp(tjs_uint16 vk, int shift) override;
	void OnKeyPress(tjs_uint16 vk, int repeat,
		bool prevkeystate, bool convertkey) override;
	tTVPImeMode GetDefaultImeMode() const override;
	void SetImeMode(tTVPImeMode mode) override;
	void ResetImeMode() override;
	void UpdateWindow(tTVPUpdateType type) override;
	void SetVisibleFromScript(bool b) override;
	void SetUseMouseKey(bool b) override;
	bool GetUseMouseKey() const override;
	void ResetMouseVelocity() override;
	void ResetTouchVelocity(tjs_int id) override;
	bool GetMouseVelocity(float &x, float &y, float &speed) const override;
	void TickBeat() override;
	cocos2d::Node *GetPrimaryArea() override { return nullptr; }

	tTJSNI_Window *GetWindow() const { return m_Window; }

	static TVPWindowLayerSDL *GetActiveWindow() { return s_ActiveWindow; }
	static void SetActiveWindow(TVPWindowLayerSDL *w) { s_ActiveWindow = w; }

private:
	void GenerateMouseEvent(bool fl, bool fr, bool fu, bool fd);

	tTJSNI_Window *m_Window;
	bool m_Visible;
	tjs_int m_Width, m_Height;
	tjs_int m_LastMouseX, m_LastMouseY;
	std::string m_Caption;
	bool m_InModal;
	int m_ModalResult;
	tjs_int m_ZoomNumer, m_ZoomDenom;
	bool m_UseMouseKey;
	bool m_MouseLeftEmulated, m_MouseRightEmulated;
	tjs_uint m_LastMouseKeyTick;
	int m_MouseKeyXAccel, m_MouseKeyYAccel;

	TVPWindowLayerSDL *m_Prev, *m_Next;
	static TVPWindowLayerSDL *s_ActiveWindow;
	static TVPWindowLayerSDL *s_LastWindow;

	friend void TVPForwardTouchBegin(int, float, float);
	friend void TVPForwardTouchEnd(int, float, float);
	friend void TVPForwardTouchMove(int, float, float);
	friend void TVPForwardTouchCancel(int, float, float);
	friend void TVPForwardTextInput(const std::string &);
	friend void TVPForwardKeyEvent(int, bool);
};

// Input forwarding helpers (called from JNI bridge)
void TVPForwardKeyEvent(int keyCode, bool isPress);
void TVPForwardTouchBegin(int id, float x, float y);
void TVPForwardTouchEnd(int id, float x, float y);
void TVPForwardTouchMove(int id, float x, float y);
void TVPForwardTouchCancel(int id, float x, float y);
void TVPForwardTextInput(const std::string &text);
void TVPSetScreenSizeFromSDL(int w, int h);
void TVPEngineTick();

// Software display init (SDL_Renderer, called from SDL_main)
bool TVPInitDisplay(struct SDL_Window *win);
