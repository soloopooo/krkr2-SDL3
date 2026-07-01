#pragma once

#include <cstdint>

void TVPInitSDL();
void TVPProcessSDLEvents();
void TVPSDLSetScreenSize(int w, int h);
int TVPSDLGetScreenWidth();
int TVPSDLGetScreenHeight();

// Virtual cursor state
extern bool g_mouseMode;
extern float g_cursorXf, g_cursorYf;
inline int g_cursorX() { return (int)(g_cursorXf + 0.5f); }
inline int g_cursorY() { return (int)(g_cursorYf + 0.5f); }

// Game surface dimensions (set by TVPSDLSetScreenSize)
extern int g_gameW, g_gameH;
extern int s_ScreenWidth, s_ScreenHeight;

// Buffer centering offset (non-zero when primaryLayer > paintBox, e.g. fullscreen)
extern int g_bufferOffsetX;

// Called from TVPEngineTick to forward cursor position to Java overlay
void TVPUpdateCursorOverlay();
