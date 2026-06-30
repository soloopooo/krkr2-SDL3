#pragma once
#include "tjsTypes.h"
void TVPThrowExceptionMessage(const tjs_char *msg);
void TVPThrowExceptionMessage(const tjs_char *msg, const tjs_char *p1);
#define TVPInternalError TJS_W("Internal Error")
