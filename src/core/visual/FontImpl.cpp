#include "FontImpl.h"
#ifdef __ANDROID__
#include <android/log.h>
#define LOG_FONT(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "##krkr_font", fmt, ##__VA_ARGS__)
#else
#define LOG_FONT(fmt, ...)
#endif

// ## fix error: unknown type name 'FT_Library'
#include "freetype2/ft2build.h"
#include "freetype2/freetype.h"
#include "freetype2/ftsnames.h"
#include "freetype2/ttnameid.h"
// #include FT_TRUETYPE_IDS_H
// #include FT_SFNT_NAMES_H
// #include FT_FREETYPE_H

#include "StorageIntf.h"
#include "DebugIntf.h"
#include "MsgIntf.h"
#include <map>
#include <math.h>
#include <sys/stat.h>
#include "Application.h"
#include "Platform.h"
#include "ConfigManager/IndividualConfigManager.h"
#ifndef M_PI
#define M_PI       3.14159265358979323846
#endif

#ifdef _MSC_VER
#pragma comment(lib,"freetype.lib")
#endif
#ifdef KRKR2_SDL_BUILD
#include <sys/stat.h>
#include <fstream>
#else
#include "platform/CCFileUtils.h"
#endif
#include "StorageImpl.h"
#include "BinaryStream.h"

tTJSHashTable<ttstr, TVPFontNamePathInfo, tTVPttstrHash>
    TVPFontNames;
static ttstr TVPDefaultFontName;
const ttstr &TVPGetDefaultFontName() {
	return TVPDefaultFontName;
}
void TVPGetAllFontList(std::vector<ttstr>& list) {
	auto itend = TVPFontNames.GetLast();
	for (auto it = TVPFontNames.GetFirst(); it != itend; ++it) {
		list.push_back(it.GetKey());
	}
}
static FT_Library TVPFontLibrary;
FT_Library &TVPGetFontLibrary() {
	if (!TVPFontLibrary) {
		FT_Error error = FT_Init_FreeType(&TVPFontLibrary);
		if (error) TVPThrowExceptionMessage(
			(ttstr(TJS_W("Initialize FreeType failed, error = ")) + TJSIntegerToString((tjs_int)error)).c_str());
	}
	return TVPFontLibrary;
}
void TVPReleaseFontLibrary() {
	if (TVPFontLibrary) {
		FT_Done_FreeType(TVPFontLibrary);
	}
}
//---------------------------------------------------------------------------
static int TVPInternalEnumFonts(FT_Byte* pBuf, int buflen, const ttstr &FontPath, const std::function<tTJSBinaryStream*(TVPFontNamePathInfo*)>& getter) {
	unsigned int faceCount = 0;
	FT_Face fontface;
	FT_Error error = FT_New_Memory_Face(
		TVPGetFontLibrary(),
		pBuf,
		buflen,
		0,
		&fontface);
	if (error) {
		TVPAddLog(ttstr(TJS_W("Load Font \"") + FontPath + "\" failed (" + TJSIntegerToString((int)error) + ")"));
		return faceCount;
	}
	int nFaceNum = fontface->num_faces;
	for (int i = 0; i < nFaceNum; ++i) {
		if (i > 0) {
			if (FT_New_Memory_Face(
				TVPGetFontLibrary(),
				pBuf,
				buflen,
				i,
				&fontface)) {
				continue;
			}
		}
		if (FT_IS_SCALABLE(fontface)) {
			FT_UInt namecount = FT_Get_Sfnt_Name_Count(fontface);
			int addCount = 0;
			for (FT_UInt i = 0; i < namecount; ++i) {
				FT_SfntName name;
				if (FT_Get_Sfnt_Name(fontface, i, &name)) {
					continue;
				}
				if (name.name_id != TT_NAME_ID_FONT_FAMILY) {
					continue;
				}
				if (name.platform_id != TT_PLATFORM_MICROSOFT) {
					continue;
				}
				switch (name.language_id) { // for CJK names
				case TT_MS_LANGID_JAPANESE_JAPAN:
				case TT_MS_LANGID_CHINESE_GENERAL:
				case TT_MS_LANGID_CHINESE_TAIWAN:
				case TT_MS_LANGID_CHINESE_PRC:
				case TT_MS_LANGID_CHINESE_HONG_KONG:
				case TT_MS_LANGID_CHINESE_SINGAPORE:
				case TT_MS_LANGID_KOREAN_EXTENDED_WANSUNG_KOREA:
				case TT_MS_LANGID_KOREAN_JOHAB_KOREA:
					break;
				default:
					continue;
				}
				ttstr fontname;
				if (name.encoding_id == TT_MS_ID_UNICODE_CS) {
					std::vector<tjs_char> tmp;
					int namelen = name.string_len / 2;
					tmp.resize(namelen + 1);
					for (int j = 0; j < namelen; ++j) {
						tmp[j] = (name.string[j * 2] << 8) | (name.string[j * 2 + 1]);
					}
					fontname = &tmp.front();
				} else {
					continue;
				}
				TVPFontNamePathInfo info;
				info.Path = FontPath;
				info.Index = i;
				info.Getter = getter;
				TVPFontNames.Add(fontname, info);
				addCount = 1;
			}
			/*if (!addCount)*/ {
				ttstr fontname((tjs_nchar*)fontface->family_name);
				TVPFontNamePathInfo info;
				info.Path = FontPath;
				info.Index = i;
				info.Getter = getter;
				TVPFontNames.Add(fontname, info);
			}
			++faceCount;
		}

		FT_Done_Face(fontface);
	}
	return faceCount;
}

int TVPEnumFontsProc(const ttstr &FontPath)
{
    if(!TVPIsExistentStorageNoSearch(FontPath)) {
		LOG_FONT("TVPEnumFontsProc: '%s' not exist", FontPath.AsNarrowStdString().c_str());
        return 0;
    }
	LOG_FONT("TVPEnumFontsProc: '%s' exists, creating stream", FontPath.AsNarrowStdString().c_str());

    tTJSBinaryStream * Stream = TVPCreateStream(FontPath, TJS_BS_READ);
    if(!Stream) {
		LOG_FONT("TVPEnumFontsProc: stream null");
        return 0;
    }
    int bufflen = Stream->GetSize();
	LOG_FONT("TVPEnumFontsProc: stream size=%d", bufflen);
	std::vector<FT_Byte> buf; buf.resize(bufflen);
    Stream->ReadBuffer(&buf.front(), bufflen);
    delete Stream;
	LOG_FONT("TVPEnumFontsProc: calling TVPInternalEnumFonts");
	int ret = TVPInternalEnumFonts(&buf.front(), bufflen, FontPath, nullptr);
	LOG_FONT("TVPEnumFontsProc: done, registered=%d", ret);
	return ret;
}

tTJSBinaryStream* TVPCreateFontStream(const ttstr &fontname)
{
	TVPFontNamePathInfo *info = TVPFindFont(fontname);
	if (!info) {
		info = TVPFontNames.Find(TVPDefaultFontName);
		if (!info) return nullptr;
	}
	if (info->Getter) {
		return info->Getter(info);
	}
	return TVPCreateBinaryStreamForRead(info->Path, TJS_W(""));
}

//---------------------------------------------------------------------------
#ifdef __ANDROID__
extern std::vector<ttstr> Android_GetExternalStoragePath();
extern std::string Android_GetApkStoragePath();
#endif
extern void TVPInializeFontRasterizers();
extern std::string Android_GetInternalStoragePath();
void TVPInitFontNames()
{
    static bool TVPFontNamesInit = false;
    // enumlate all fonts
    if(TVPFontNamesInit) return;
	TVPFontNamesInit = true;
	LOG_FONT("TVPInitFontNames: start");

	TVPInializeFontRasterizers(); // ensure TVPFontSystem exists before any font operation
	LOG_FONT("TVPInitFontNames: TVPInializeFontRasterizers done");
	TVPGetFontLibrary(); // initialize FreeType before font enumeration (prevent recursive crash)
	LOG_FONT("TVPInitFontNames: TVPGetFontLibrary/FreeType initialized");
#ifdef __ANDROID__
	std::vector<ttstr> pathlist = Android_GetExternalStoragePath();
	LOG_FONT("TVPInitFontNames: got pathlist size=%zu", pathlist.size());
#endif
	do {
		ttstr userFont = IndividualConfigManager::GetInstance()->GetValue<std::string>("default_font", "");
		LOG_FONT("TVPInitFontNames: userFont=%s", userFont.IsEmpty() ? "(empty)" : userFont.AsNarrowStdString().c_str());
		if (!userFont.IsEmpty() && TVPEnumFontsProc(userFont)) break;

		LOG_FONT("TVPInitFontNames: trying apppath/default.ttf");
		if (TVPEnumFontsProc(TVPGetAppPath() + "default.ttf")) { LOG_FONT("TVPInitFontNames: FOUND default.ttf at apppath"); break; }
		LOG_FONT("TVPInitFontNames: trying apppath/default.ttc");
		if (TVPEnumFontsProc(TVPGetAppPath() + "default.ttc")) { LOG_FONT("TVPInitFontNames: FOUND default.ttc"); break; }
		LOG_FONT("TVPInitFontNames: trying apppath/default.otf");
		if (TVPEnumFontsProc(TVPGetAppPath() + "default.otf")) { LOG_FONT("TVPInitFontNames: FOUND default.otf"); break; }
		LOG_FONT("TVPInitFontNames: trying apppath/default.otc");
		if (TVPEnumFontsProc(TVPGetAppPath() + "default.otc")) { LOG_FONT("TVPInitFontNames: FOUND default.otc"); break; }
#if defined(__ANDROID__)
		int fontCount = 0;
		for (size_t pi = 0; pi < pathlist.size(); pi++) {
			LOG_FONT("TVPInitFontNames: trying external %s/default.ttf", pathlist[pi].AsNarrowStdString().c_str());
			fontCount += TVPEnumFontsProc(pathlist[pi] + "/default.ttf");
			if (fontCount) { LOG_FONT("TVPInitFontNames: FOUND default.ttf at external path"); break; }
		}
		if (fontCount) break;

		{
			std::string internalPath = Android_GetInternalStoragePath();
			LOG_FONT("TVPInitFontNames: internal path='%s' trying /default.ttf", internalPath.c_str());
			if (TVPEnumFontsProc(ttstr(internalPath) + "/default.ttf")) { LOG_FONT("TVPInitFontNames: FOUND internal/default.ttf"); break; }
#ifdef KRKR2_SDL_BUILD
			// Also try DroidSansFallback.ttf (extracted from APK assets by KR2Activity.onCreate)
			// Use direct stat/fopen since TVP storage may not handle absolute paths
			{
				std::string fallbackPath = internalPath + "/DroidSansFallback.ttf";
				LOG_FONT("TVPInitFontNames: trying %s", fallbackPath.c_str());
				struct stat st;
				if (stat(fallbackPath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
					FILE *fp = fopen(fallbackPath.c_str(), "rb");
					if (fp) {
						fseek(fp, 0, SEEK_END);
						long sz = ftell(fp);
						fseek(fp, 0, SEEK_SET);
						std::vector<FT_Byte> fbuf(sz);
						fread(fbuf.data(), 1, sz, fp);
						fclose(fp);
						if (TVPInternalEnumFonts(fbuf.data(), sz, ttstr(fallbackPath),
							[fallbackPath](TVPFontNamePathInfo*)->tTJSBinaryStream* {
								FILE *fp2 = fopen(fallbackPath.c_str(), "rb");
								if (!fp2) return nullptr;
								fseek(fp2, 0, SEEK_END);
								long sz2 = ftell(fp2);
								fseek(fp2, 0, SEEK_SET);
								tTVPMemoryStream *ms = new tTVPMemoryStream();
								ms->SetSize(sz2);
								fread(ms->GetInternalBuffer(), 1, sz2, fp2);
								fclose(fp2);
								return ms;
							})) {
							LOG_FONT("TVPInitFontNames: FOUND internal/DroidSansFallback.ttf");
							fontCount = 1;
						}
					}
				}
				if (fontCount) break;
			}
#endif
		}

#ifndef KRKR2_SDL_BUILD
		{	// from APK assets (cocos2d path)
			auto data = cocos2d::FileUtils::getInstance()->getDataFromFile("DroidSansFallback.ttf");
			if (TVPInternalEnumFonts(data.getBytes(), data.getSize(), "DroidSansFallback.ttf", [](TVPFontNamePathInfo* info)->tTJSBinaryStream* {
				auto data = cocos2d::FileUtils::getInstance()->getDataFromFile(info->Path.AsStdString());
				tTVPMemoryStream *ret = new tTVPMemoryStream();
				ret->WriteBuffer(data.getBytes(), data.getSize());
				ret->SetPosition(0);
				return ret;
			})) break;
		}
#endif

#ifdef KRKR2_SDL_BUILD
		// SDL2: try system font directories with absolute paths.
		// Use direct stat/open since TVP storage system does not handle absolute paths.
		{
			// First, try to extract DroidSansFallback.ttf from APK assets if present
			{
				std::string apkPath = Android_GetApkStoragePath();
				if (!apkPath.empty()) {
					// Try to open the APK as a ZIP and read assets/DroidSansFallback.ttf
					// by using the archive stream system
					ttstr arcPath = ttstr(apkPath) + TJS_W(">assets/DroidSansFallback.ttf");
					if (TVPIsExistentStorageNoSearchNoNormalize(arcPath)) {
						LOG_FONT("TVPInitFontNames: found DroidSansFallback.ttf in APK");
						if (TVPEnumFontsProc(arcPath)) { fontCount = 1; }
					}
					if (fontCount) { LOG_FONT("TVPInitFontNames: FOUND DroidSansFallback.ttf from APK"); break; }
				}
			}
			const char *sysfonts[] = {
				"/system/fonts/MiSansLatinVF.ttf",
				"/system/fonts/MiSansJapaneseVF.ttf",
				"/system/fonts/MiSansTCVF.ttf",
				"/system/fonts/DroidSans.ttf",
				"/system/fonts/DroidSans-Bold.ttf",
			};
			for (auto p : sysfonts) {
				LOG_FONT("TVPInitFontNames: trying sysfont %s", p);
				struct stat st;
				if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
				FILE *fp = fopen(p, "rb");
				if (!fp) continue;
				fseek(fp, 0, SEEK_END);
				long size = ftell(fp);
				fseek(fp, 0, SEEK_SET);
				std::vector<FT_Byte> buf(size);
				fread(buf.data(), 1, size, fp);
				fclose(fp);
				ttstr fontPath(p);
				// Register with a Getter that opens the font file directly (bypass storage system)
				int n = TVPInternalEnumFonts(buf.data(), size, fontPath,
					[](TVPFontNamePathInfo* info)->tTJSBinaryStream* {
						std::string path = info->Path.AsNarrowStdString();
						FILE *fp = fopen(path.c_str(), "rb");
						if (!fp) return nullptr;
						fseek(fp, 0, SEEK_END);
						long sz = ftell(fp);
						fseek(fp, 0, SEEK_SET);
						tTVPMemoryStream *ms = new tTVPMemoryStream();
						ms->SetSize(sz);
						fread(ms->GetInternalBuffer(), 1, sz, fp);
						fclose(fp);
						return ms;
					});
				LOG_FONT("TVPInitFontNames: sysfont %s registered=%d", p, n);
				if (n) { fontCount = 1; break; }
			}
			if (fontCount) break;
		}
#else
		if (TVPEnumFontsProc(TJS_W("file://./system/fonts/DroidSansFallback.ttf"))) break;
		if (TVPEnumFontsProc(TJS_W("file://./system/fonts/NotoSansHans-Regular.otf"))) break;
		if (TVPEnumFontsProc(TJS_W("file://./system/fonts/DroidSans.ttf"))) break;
#endif

#elif defined(WIN32)
		if (TVPEnumFontsProc(TJS_W("file://./c/windows/fonts/msyh.ttf"))) break;
		if (TVPEnumFontsProc(TJS_W("file://./c/windows/fonts/simhei.ttf"))) break;
#endif
        
#ifndef KRKR2_SDL_BUILD
        std::string fullPath = cocos2d::FileUtils::getInstance()->fullPathForFilename("DroidSansFallback.ttf");
        if (TVPEnumFontsProc(fullPath)) break;
#endif
	} while (false);
    if(TVPFontNames.GetCount() > 0)
    {
        // set default fontface name
        TVPDefaultFontName = TVPFontNames.GetLast().GetKey();
    }

    // check exePath + "/fonts/*.ttf"
	{
		std::vector<ttstr> list;
		auto lister = [&](const ttstr &name, tTVPLocalFileInfo* s) {
			if (s->Mode & (S_IFREG | S_IFDIR)) {
				list.emplace_back(name);
			}
		};
#ifdef __ANDROID__
		TVPGetLocalFileListAt(Android_GetInternalStoragePath() + "/fonts", lister);
		for (const ttstr &path : pathlist) {
			TVPGetLocalFileListAt(path + "/fonts", lister);
		}
#endif
		TVPGetLocalFileListAt(TVPGetAppPath() + "/fonts", lister);
        auto itend = list.end();
        for (auto it = list.begin(); it != itend; ++it) {
            TVPEnumFontsProc(*it);
        }
    }

	if (TVPDefaultFontName.IsEmpty()) {
		LOG_FONT("TVPInitFontNames: FAILED - no fonts found");
		TVPShowSimpleMessageBox(("Could not found any font.\nPlease ensure that at least \"default.ttf\" exists"), "Exception Occured");
    } else {
		LOG_FONT("TVPInitFontNames: done, default font = %s", TVPDefaultFontName.AsNarrowStdString().c_str());
	}
}
//---------------------------------------------------------------------------
TVPFontNamePathInfo* TVPFindFont(const ttstr &fontname)
{
    // check existence of font
    TVPInitFontNames();

	TVPFontNamePathInfo *info = nullptr;
	if (!fontname.IsEmpty() && fontname[0] == TJS_W('@')) { // vertical version
		info = TVPFontNames.Find(fontname.c_str() + 1);
	}
	if (!info) {
		info = TVPFontNames.Find(fontname);
	}
    return info;
}

tjs_uint32 tTVPttstrHash::Make( const ttstr &val )
{
    const tjs_char * ptr = val.c_str();
    if(*ptr == 0) return 0;
    tjs_uint32 v = 0;
    while(*ptr)
    {
        v += *ptr;
        v += (v << 10);
        v ^= (v >> 6);
        ptr++;
    }
    v += (v << 3);
    v ^= (v >> 11);
    v += (v << 15);
    if(!v) v = (tjs_uint32)-1;
    return v;
}
