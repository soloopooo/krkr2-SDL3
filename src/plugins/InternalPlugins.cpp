#include "ncbind/ncbind.hpp"
//extern void InitPlugin_CSVParser();

void TVPLoadInternalPlugins()
{
	ncbAutoRegister::AllRegist();
	ncbAutoRegister::LoadModule(TJS_W("addFont.dll"));
	ncbAutoRegister::LoadModule(TJS_W("csvParser.dll"));
	ncbAutoRegister::LoadModule(TJS_W("dirlist.dll"));
	ncbAutoRegister::LoadModule(TJS_W("fftgraph.dll"));
	ncbAutoRegister::LoadModule(TJS_W("getSample.dll"));
	ncbAutoRegister::LoadModule(TJS_W("getabout.dll"));
	ncbAutoRegister::LoadModule(TJS_W("layerExDraw.dll"));
	ncbAutoRegister::LoadModule(TJS_W("layerExMovie.dll"));
	ncbAutoRegister::LoadModule(TJS_W("perspective.dll"));
	ncbAutoRegister::LoadModule(TJS_W("saveStruct.dll"));
	ncbAutoRegister::LoadModule(TJS_W("scriptsEx.dll"));
	ncbAutoRegister::LoadModule(TJS_W("varfile.dll"));
	ncbAutoRegister::LoadModule(TJS_W("win32dialog.dll"));
	ncbAutoRegister::LoadModule(TJS_W("windowEx.dll"));
	ncbAutoRegister::LoadModule(TJS_W("wutcwf.dll"));
	ncbAutoRegister::LoadModule(TJS_W("xp3filter.dll"));
}

void TVPUnloadInternalPlugins()
{
    ncbAutoRegister::AllUnregist();
}

bool TVPLoadInternalPlugin(const ttstr &_name)
{
	return ncbAutoRegister::LoadModule(TVPExtractStorageName(_name));
}