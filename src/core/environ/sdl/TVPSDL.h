#pragma once

#include <cstdint>

void TVPInitSDL();
void TVPProcessSDLEvents();
void TVPSDLSetScreenSize(int w, int h);
int TVPSDLGetScreenWidth();
int TVPSDLGetScreenHeight();

// Virtual cursor state
extern bool g_mouseMode;
extern int g_cursorX, g_cursorY;

// Game surface dimensions (set by TVPSDLSetScreenSize)
extern int g_gameW, g_gameH;
extern int s_ScreenWidth, s_ScreenHeight;

// Called from TVPEngineTick to forward cursor position to Java overlay
void TVPUpdateCursorOverlay();
