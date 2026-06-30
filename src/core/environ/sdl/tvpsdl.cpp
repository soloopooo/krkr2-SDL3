//---------------------------------------------------------------------------
// SDL3 integration layer — event pump + input router
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "SDL3/SDL.h"

#include "Application.h"
#include "DebugIntf.h"
#include "WindowLayer_sdl.h"
#include "vkdefine.h"

static bool sSDLInited = false;
static int sScreenWidth = 1280;
static int sScreenHeight = 720;

void TVPInitSDL() {
	if (sSDLInited) return;
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
		TVPAddImportantLog(ttstr(TJS_W("SDL_Init failed: ")) +
			ttstr(SDL_GetError()));
		return;
	}
	sSDLInited = true;
	TVPAddImportantLog(TJS_W("SDL3 initialized (video+events+timer)"));
}

void TVPSDLSetScreenSize(int w, int h) {
	sScreenWidth = w;
	sScreenHeight = h;
}
int TVPSDLGetScreenWidth() { return sScreenWidth; }
int TVPSDLGetScreenHeight() { return sScreenHeight; }

static int _mapSDLK2VK(int sym) {
	switch (sym) {
	case SDLK_AC_BACK: return VK_ESCAPE;
	case SDLK_MENU: return VK_MENU;
	case SDLK_UP: return VK_UP;
	case SDLK_DOWN: return VK_DOWN;
	case SDLK_LEFT: return VK_LEFT;
	case SDLK_RIGHT: return VK_RIGHT;
	case SDLK_RETURN: return VK_RETURN;
	case SDLK_DELETE: return VK_BACK;
	case SDLK_SPACE: return VK_SPACE;
	case SDLK_ESCAPE: return VK_ESCAPE;
	case SDLK_TAB: return VK_TAB;
	case SDLK_HOME: return VK_HOME;
	case SDLK_END: return VK_END;
	case SDLK_PAGEUP: return VK_PRIOR;
	case SDLK_PAGEDOWN: return VK_NEXT;
	case SDLK_INSERT: return VK_INSERT;
	default: return sym;
	}
}

void TVPProcessSDLEvents() {
	if (!sSDLInited) return;
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_EVENT_QUIT:
			::Application->Terminate();
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			::Application->OnActivate();
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			::Application->OnDeactivate();
			break;
		case SDL_EVENT_WINDOW_RESIZED:
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
			TVPSetScreenSizeFromSDL(e.window.data1, e.window.data2);
			break;
		// Touch events (primary on Android)
		case SDL_EVENT_FINGER_DOWN:
			TVPForwardTouchBegin(0,
				e.tfinger.x * sScreenWidth,
				e.tfinger.y * sScreenHeight);
			break;
		case SDL_EVENT_FINGER_UP:
			TVPForwardTouchEnd(0,
				e.tfinger.x * sScreenWidth,
				e.tfinger.y * sScreenHeight);
			break;
		case SDL_EVENT_FINGER_CANCELED:
			TVPForwardTouchCancel(0,
				e.tfinger.x * sScreenWidth,
				e.tfinger.y * sScreenHeight);
			break;
		case SDL_EVENT_FINGER_MOTION:
			TVPForwardTouchMove(0,
				e.tfinger.x * sScreenWidth,
				e.tfinger.y * sScreenHeight);
			break;
		// Mouse events (USB/Bluetooth mouse, stylus hover)
		case SDL_EVENT_MOUSE_MOTION:
			TVPForwardTouchMove(0, e.motion.x, e.motion.y);
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			TVPForwardTouchBegin(0, e.button.x, e.button.y);
			break;
		case SDL_EVENT_MOUSE_BUTTON_UP:
			TVPForwardTouchEnd(0, e.button.x, e.button.y);
			break;
		case SDL_EVENT_KEY_DOWN:
			TVPForwardKeyEvent(_mapSDLK2VK(e.key.key), true);
			break;
		case SDL_EVENT_KEY_UP:
			TVPForwardKeyEvent(_mapSDLK2VK(e.key.key), false);
			break;
		case SDL_EVENT_TEXT_INPUT:
			TVPForwardTextInput(e.text.text);
			break;
		default:
			break;
		}
	}
}
