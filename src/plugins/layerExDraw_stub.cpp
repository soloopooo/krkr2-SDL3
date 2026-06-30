#include "ncbind/ncbind.hpp"

#define NCB_MODULE_NAME TJS_W("layerExDraw.dll")

struct GdiPlus {
	static void addPrivateFont(const tjs_char *) {}
	static tTJSVariant getFontList(bool) { return tTJSVariant(); }
};

struct GdiPlusAppearance {
};

NCB_REGISTER_CLASS(GdiPlus) {
	Variant(TJS_W("Ok"), (int)0);
	NCB_METHOD(addPrivateFont);
	NCB_METHOD(getFontList);
	NCB_SUBCLASS(Appearance, GdiPlusAppearance);
}

NCB_REGISTER_SUBCLASS(GdiPlusAppearance) {
	Constructor();
}

struct LayerExDrawStub {
	static tjs_error TJS_INTF_METHOD drawString(tTJSVariant *result, tjs_int, tTJSVariant **, iTJSDispatch2 *) {
		TVPThrowExceptionMessage(TJS_W("layerExDraw (GdiPlus) is not available on this platform."));
		if (result) result->Clear();
		return TJS_E_FAIL;
	}
	static tjs_error TJS_INTF_METHOD registerExEvent(tTJSVariant *result, tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis) {
		if (result) result->Clear();
		return TJS_S_OK;
	}
};

NCB_ATTACH_CLASS_WITH_HOOK(LayerExDrawStub, Layer) {
	RawCallback(TJS_W("drawString"), &LayerExDrawStub::drawString, 0);
	RawCallback(TJS_W("registerExEvent"), &LayerExDrawStub::registerExEvent, 0);
}
