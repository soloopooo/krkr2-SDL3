#include "cocos2d.h"
#include "tjsCommHead.h"

#include "TVPScreen.h"
#include "Application.h"

int tTVPScreen::GetWidth() {
#ifdef KRKR2_SDL_BUILD
	extern int s_ScreenWidth;
	return s_ScreenWidth > 0 ? s_ScreenWidth : 1280;
#else
	return 2048;
#endif
}
int tTVPScreen::GetHeight() {
#ifdef KRKR2_SDL_BUILD
	extern int s_ScreenHeight;
	return s_ScreenHeight > 0 ? s_ScreenHeight : 720;
#else
	const cocos2d::Size &size = cocos2d::Director::getInstance()->getOpenGLView()->getFrameSize();
	int w = GetWidth();
	int h = w * (size.height / size.width);
	return w;
#endif
}

int tTVPScreen::GetDesktopLeft() {
	return 0;
}
int tTVPScreen::GetDesktopTop() {
	return 0;
}
int tTVPScreen::GetDesktopWidth() {
	return GetWidth();
}
int tTVPScreen::GetDesktopHeight() {
	return GetHeight();
}

