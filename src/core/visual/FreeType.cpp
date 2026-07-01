//---------------------------------------------------------------------------
/*
	Risa [�肳]      alias �g���g��3 [kirikiri-3]
	 stands for "Risa Is a Stagecraft Architecture"
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
//! @file
//! @brief FreeType �t�H���g�h���C�o
//---------------------------------------------------------------------------

#include "tjsCommHead.h"
#include "FreeType.h"
//#include "NativeFreeTypeFace.h"
//#include "uni_cp932.h"
//#include "cp932_uni.h"

#include "BinaryStream.h"
#include "MsgIntf.h"
#include "SysInitIntf.h"
#include "ComplexRect.h"

#include <algorithm>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4819)
#endif
#include <ft2build.h>
#include FT_TRUETYPE_UNPATENTED_H
#include FT_SYNTHESIS_H
#include FT_BITMAP_H
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include "FontImpl.h"

extern bool TVPEncodeUTF8ToUTF16(ttstr &output, const std::string &source);

//---------------------------------------------------------------------------

FT_Library FreeTypeLibrary = NULL;	//!< FreeType ���C�u����
void TVPInitializeFont() {
	if( FreeTypeLibrary == NULL ) {
		FT_Error err = FT_Init_FreeType( &FreeTypeLibrary );
	}
}
void TVPUninitializeFreeFont() {
	if( FreeTypeLibrary ) {
		FT_Done_FreeType( FreeTypeLibrary );
		FreeTypeLibrary = NULL;
	}
}

//---------------------------------------------------------------------------
/**
 * �t�@�C���V�X�e���o�R�ł�FreeType Face �N���X
 */
class tGenericFreeTypeFace : public tBaseFreeTypeFace
{
protected:
	FT_Face Face;	//!< FreeType face �I�u�W�F�N�g
	tTJSBinaryStream* File;	 //!< tTJSBinaryStream �I�u�W�F�N�g
	std::vector<ttstr> FaceNames; //!< Face����񋓂����z��

private:
	FT_StreamRec Stream;

public:
	tGenericFreeTypeFace(const ttstr &fontname, tjs_uint32 options);
	virtual ~tGenericFreeTypeFace();

	virtual FT_Face GetFTFace() const;
	virtual void GetFaceNameList(std::vector<ttstr> & dest) const;
	virtual tjs_char GetDefaultChar() const { return L' '; }

private:
	void Clear();
	static unsigned long IoFunc( FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count );
	static void CloseFunc( FT_Stream  stream );

	bool OpenFaceByIndex(tjs_uint index, FT_Face & face);
};
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
/**
 * �R���X�g���N�^
 * @param fontname	�t�H���g��
 * @param options	�I�v�V����(TVP_TF_XXXX �萔��TVP_FACE_OPTIONS_XXXX�萔�̑g�ݍ��킹)
 */
tGenericFreeTypeFace::tGenericFreeTypeFace(const ttstr &fontname, tjs_uint32 options) : File(NULL)
{
	// �t�B�[���h�̏�����
	Face = NULL;
	memset(&Stream, 0, sizeof(Stream));

	try {
		if(File) {
			delete File;
			File = NULL;
		} 

		// �t�@�C�����J��
		if (options & TVP_FACE_OPTIONS_FILE) {
			File = TVPCreateBinaryStreamForRead(fontname, TJS_W(""));
		} else {
			File = TVPCreateFontStream(fontname);
		}
		if( File == NULL ) {
			TVPThrowExceptionMessage( TVPCannotOpenFontFile, fontname );
		}

		// FT_StreamRec �̊e�t�B�[���h�𖄂߂�
		FT_StreamRec * fsr = &Stream;
		fsr->base = 0;
		fsr->size = static_cast<unsigned long>(File->GetSize());
		fsr->pos = 0;
		fsr->descriptor.pointer = this;
		fsr->pathname.pointer = NULL;
		fsr->read = IoFunc;
		fsr->close = CloseFunc;

		// Face �����ꂼ��J���AFace�����擾���� FaceNames �Ɋi�[����
		tjs_uint face_num = 1;

		FT_Face face = NULL;

		for(tjs_uint i = 0; i < face_num; i++)
		{
			if(!OpenFaceByIndex(i, face))
			{
				FaceNames.push_back(ttstr());
			}
			else
			{
				const char * name = face->family_name;
				ttstr wname;
				TVPEncodeUTF8ToUTF16( wname, std::string(name) );
				FaceNames.push_back( wname );
				face_num = face->num_faces;
			}
		}

		if(face) FT_Done_Face(face), face = NULL;


		// FreeType �G���W���Ńt�@�C�����J�����Ƃ��Ă݂�
		tjs_uint index = TVP_GET_FACE_INDEX_FROM_OPTIONS(options);
		if(!OpenFaceByIndex(index, Face)) {
			// �t�H���g���J���Ȃ�����
			TVPThrowExceptionMessage(TVPFontCannotBeUsed, fontname );
		}
	}
	catch(...)
	{
		throw;
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
/**
 * �f�X�g���N�^
 */
tGenericFreeTypeFace::~tGenericFreeTypeFace()
{
	if(Face) FT_Done_Face(Face), Face = NULL;
	if(File) {
		delete File;
		File = NULL;
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
/**
 * FreeType �� Face �I�u�W�F�N�g��Ԃ�
 */
FT_Face tGenericFreeTypeFace::GetFTFace() const
{
	return Face;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
/**
 * ���̃t�H���g�t�@�C���������Ă���t�H���g��z��Ƃ��ĕԂ�
 */
void tGenericFreeTypeFace::GetFaceNameList(std::vector<ttstr> & dest) const
{
	dest = FaceNames;
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
/**
 * FreeType �p �X�g���[���ǂݍ��݊֐�
 */
unsigned long tGenericFreeTypeFace::IoFunc( FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count )
{
	tGenericFreeTypeFace * _this =
		static_cast<tGenericFreeTypeFace*>(stream->descriptor.pointer);

	size_t result;
	if(count == 0)
	{
		// seek
		result = 0;
		_this->File->SetPosition( offset );
	}
	else
	{
		// read
		_this->File->SetPosition( offset );
		_this->File->ReadBuffer(buffer, count);
		result = count;
	}

	return (unsigned long)result;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
/**
 * FreeType �p �X�g���[���폜�֐�
 */
void tGenericFreeTypeFace::CloseFunc( FT_Stream  stream )
{
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
/**
 * �w��C���f�b�N�X��Face���J��
 * @param index	�J��index
 * @param face	FT_Face �ϐ��ւ̎Q��
 * @return	Face���J����� true �����łȂ���� false
 * @note	���߂� Face ���J���ꍇ�� face �Ŏw�肷��ϐ��ɂ� null �����Ă�������
 */
bool tGenericFreeTypeFace::OpenFaceByIndex(tjs_uint index, FT_Face & face)
{
	if(face) FT_Done_Face(face), face = NULL;

	FT_Parameter parameters[1];
	parameters[0].tag = FT_PARAM_TAG_UNPATENTED_HINTING; // Apple�̓���������s��
	parameters[0].data = NULL;

	FT_Open_Args args;
	memset(&args, 0, sizeof(args));
	args.flags = FT_OPEN_STREAM;
	args.stream = &Stream;
	args.driver = 0;
	args.num_params = 1;
	args.params = parameters;

	FT_Error err = FT_Open_Face( FreeTypeLibrary, &args, index, &face);

	return err == 0;
}
//---------------------------------------------------------------------------





//---------------------------------------------------------------------------
/**
 * �R���X�g���N�^
 * @param fontname	�t�H���g��
 * @param options	�I�v�V����
 */
tFreeTypeFace::tFreeTypeFace(const ttstr &fontname, tjs_uint32 options)
	: FontName(fontname)
{
	TVPInitializeFont();

	// �t�B�[���h���N���A
	Face = NULL;
	GlyphIndexToCharcodeVector = NULL;
	UnicodeToLocalChar = NULL;
	LocalCharToUnicode = NULL;
	Options = options;
	Height = 10;


	// �t�H���g���J��
	//if(options & TVP_FACE_OPTIONS_FILE)
	{
		// �t�@�C�����J��
		Face = new tGenericFreeTypeFace(fontname, options);
			// ��O�������Ŕ�������\��������̂Œ���
	}
	//else
	{
		// �l�C�e�B�u�̃t�H���g���ɂ��w�� (�v���b�g�t�H�[���ˑ�)
		//Face = new tNativeFreeTypeFace(fontname, options);
			// ��O�������Ŕ�������\��������̂Œ���
	}
	FTFace = Face->GetFTFace();

	// �}�b�s���O���m�F����
	if(FTFace->charmap == NULL)
	{
		// FreeType �͎����I�� UNICODE �}�b�s���O���g�p���邪�A
		// �t�H���g�� UNICODE �}�b�s���O�̏����܂�ł��Ȃ��ꍇ��
		// �����I�ȕ����}�b�s���O�̑I���͍s���Ȃ��B
		// �Ƃ肠����(���{����Ɍ����Č�����) SJIS �}�b�s���O���������ĂȂ�
		// �t�H���g�������̂�SJIS��I�������Ă݂�B
#if 0
		FT_Error err = FT_Select_Charmap(FTFace, FT_ENCODING_SJIS);
		if(!err)
		{
			// SJIS �ւ̐؂�ւ�����������
			// �ϊ��֐����Z�b�g����
 			UnicodeToLocalChar = UnicodeToSJIS;
 			LocalCharToUnicode = SJISToUnicode;
		}
		else
#else
		FT_Error err;
#endif
		{
			int numcharmap = FTFace->num_charmaps;
			for( int i = 0; i < numcharmap; i++ )
			{
				FT_Encoding enc = FTFace->charmaps[i]->encoding;
				if( enc != FT_ENCODING_NONE && enc != FT_ENCODING_APPLE_ROMAN )
				{
					err = FT_Select_Charmap(FTFace, enc);
					if(!err) {
						break;
					}
				}
			}
		}
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
/**
 * �f�X�g���N�^
 */
tFreeTypeFace::~tFreeTypeFace()
{
	if(GlyphIndexToCharcodeVector) delete GlyphIndexToCharcodeVector;
	if(Face) delete Face;
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
/**
 * ����Face���ێ����Ă���glyph�̐��𓾂�
 * @return	����Face���ێ����Ă���glyph�̐�
 */
tjs_uint tFreeTypeFace::GetGlyphCount()
{
	if(!FTFace) return 0;

	// FreeType ���Ԃ��Ă���O���t�̐��́A���ۂɕ����R�[�h�����蓖�Ă��Ă��Ȃ�
	// �O���t�����܂񂾐��ƂȂ��Ă���
	// �����ŁA���ۂɃt�H���g�Ɋ܂܂�Ă���O���t���擾����
	// TODO:�X���b�h�ی삳��Ă��Ȃ��̂Œ��ӁI�I�I�I�I�I
	if(!GlyphIndexToCharcodeVector)
	{
		// �}�b�v���쐬����Ă��Ȃ��̂ō쐬����
		GlyphIndexToCharcodeVector = new tGlyphIndexToCharcodeVector;
		FT_ULong  charcode;
		FT_UInt   gindex;
		charcode = FT_Get_First_Char( FTFace, &gindex );
		while ( gindex != 0 )
		{
			FT_ULong code;
			if(LocalCharToUnicode)
				code = LocalCharToUnicode(charcode);
			else
				code = charcode;
			GlyphIndexToCharcodeVector->push_back(code);
			charcode = FT_Get_Next_Char( FTFace, charcode, &gindex );
		}
		std::sort(
			GlyphIndexToCharcodeVector->begin(),
			GlyphIndexToCharcodeVector->end()); // �����R�[�h���ŕ��ёւ�
	}

	return (tjs_uint)GlyphIndexToCharcodeVector->size();
}
//---------------------------------------------------------------------------



//---------------------------------------------------------------------------
/**
 * Glyph �C���f�b�N�X����Ή����镶���R�[�h�𓾂�
 * @param index	�C���f�b�N�X(FreeType�̊Ǘ����Ă��镶��index�Ƃ͈Ⴄ�̂Œ���)
 * @return	�Ή����镶���R�[�h(�Ή�����R�[�h�������ꍇ�� 0)
 */
tjs_char tFreeTypeFace::GetCharcodeFromGlyphIndex(tjs_uint index)
{
	tjs_uint size = GetGlyphCount(); // �O���t���𓾂���łɃ}�b�v���쐬����

	if(!GlyphIndexToCharcodeVector) return 0;
	if(index >= size) return 0;

	return static_cast<tjs_char>((*GlyphIndexToCharcodeVector)[index]);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
/**
 * ���̃t�H���g�Ɋ܂܂��Face���̃��X�g�𓾂�
 * @param dest	�i�[��z��
 */
void tFreeTypeFace::GetFaceNameList(std::vector<ttstr> &dest)
{
	Face->GetFaceNameList(dest);
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
/**
 * �t�H���g�̍�����ݒ肷��
 * @param height	�t�H���g�̍���(�s�N�Z���P��)
 */
void tFreeTypeFace::SetHeight(int height)
{
	Height = height;
	FT_Error err = FT_Set_Pixel_Sizes(FTFace, 0, Height);
	if(err)
	{
		// TODO: Error �n���h�����O
	}
}
//---------------------------------------------------------------------------


//---------------------------------------------------------------------------
/**
 * �w�肵�������R�[�h�ɑ΂���O���t�r�b�g�}�b�v�𓾂�
 * @param code	�����R�[�h
 * @return	�V�K�쐬���ꂽ�O���t�r�b�g�}�b�v�I�u�W�F�N�g�ւ̃|�C���^
 *			NULL �̏ꍇ�͕ϊ��Ɏ��s�����ꍇ
 */
tTVPCharacterData * tFreeTypeFace::GetGlyphFromCharcode(tjs_char code)
{
	// �O���t�X���b�g�ɃO���t��ǂݍ��݁A���@���擾����
	tGlyphMetrics metrics;
	if(!GetGlyphMetricsFromCharcode(code, metrics))
		return NULL;

	// �����������_�����O����
	FT_Error err;

	if(FTFace->glyph->format != FT_GLYPH_FORMAT_BITMAP)
	{
		FT_Render_Mode mode;
		if(!(Options & TVP_FACE_OPTIONS_NO_ANTIALIASING))
			mode = FT_RENDER_MODE_NORMAL;
		else
			mode = FT_RENDER_MODE_MONO;
		err = FT_Render_Glyph(FTFace->glyph, mode);
			// note: �f�t�H���g�̃����_�����O���[�h�� FT_RENDER_MODE_NORMAL (256�F�O���[�X�P�[��)
			//       FT_RENDER_MODE_MONO �� 1bpp ���m�N���[��
		if(err) return NULL;
	}

	// �ꉞ�r�b�g�}�b�v�`�����`�F�b�N
	FT_Bitmap *ft_bmp = &(FTFace->glyph->bitmap);
	FT_Bitmap new_bmp;
	bool release_ft_bmp = false;
	tTVPCharacterData * glyph_bmp = NULL;
	try
	{
		if(ft_bmp->rows && ft_bmp->width)
		{
			// �r�b�g�}�b�v���T�C�Y�������Ă���ꍇ
			if(ft_bmp->pixel_mode != ft_pixel_mode_grays)
			{
				// ft_pixel_mode_grays �ł͂Ȃ��̂� ft_pixel_mode_grays �`���ɕϊ�����
				FT_Bitmap_New(&new_bmp);
				release_ft_bmp = true;
				ft_bmp = &new_bmp;
				err = FT_Bitmap_Convert(FTFace->glyph->library,
					&(FTFace->glyph->bitmap),
					&new_bmp, 1);
					// ���� tGlyphBitmap �`���ɕϊ�����ۂɃA���C�������g���������̂�
					// �����Ŏw�肷�� alignment �� 1 �ł悢
				if(err)
				{
					if(release_ft_bmp) FT_Bitmap_Done(FTFace->glyph->library, ft_bmp);
					return NULL;
				}
			}

			if(ft_bmp->num_grays != 256)
			{
				// gray ���x���� 256 �ł͂Ȃ�
				// 256 �ɂȂ�悤�ɏ�Z���s��
				tjs_int32 multiply =
					static_cast<tjs_int32>((static_cast<tjs_int32> (1) << 30) - 1) /
						(ft_bmp->num_grays - 1);
				for(tjs_int y = ft_bmp->rows - 1; y >= 0; y--)
				{
					unsigned char * p = ft_bmp->buffer + y * ft_bmp->pitch;
					for(tjs_int x = ft_bmp->width - 1; x >= 0; x--)
					{
						tjs_int32 v = static_cast<tjs_int32>((*p * multiply)  >> 22);
						*p = static_cast<unsigned char>(v);
						p++;
					}
				}
			}
		}
		// 64�{����Ă�����̂���������
		metrics.CellIncX = FT_PosToInt( metrics.CellIncX );
		metrics.CellIncY = FT_PosToInt( metrics.CellIncY );

		// tGlyphBitmap ���쐬���ĕԂ�
		//int baseline = (int)(FTFace->height + FTFace->descender) * FTFace->size->metrics.y_ppem / FTFace->units_per_EM;
		int baseline = (int)( FTFace->ascender ) * FTFace->size->metrics.y_ppem / FTFace->units_per_EM;

		glyph_bmp = new tTVPCharacterData(
			ft_bmp->buffer,
			ft_bmp->pitch,
			  FTFace->glyph->bitmap_left,
			  baseline - FTFace->glyph->bitmap_top,
			  ft_bmp->width,
			  ft_bmp->rows,
			metrics);
		glyph_bmp->Gray = 256;

		
		if( Options & TVP_TF_UNDERLINE ) {
			tjs_int pos = -1, thickness = -1;
			GetUnderline( pos, thickness );
			if( pos >= 0 && thickness > 0 ) {
				glyph_bmp->AddHorizontalLine( pos, thickness, 255 );
			}
		}
		if( Options & TVP_TF_STRIKEOUT ) {
			tjs_int pos = -1, thickness = -1;
			GetStrikeOut( pos, thickness );
			if( pos >= 0 && thickness > 0 ) {
				glyph_bmp->AddHorizontalLine( pos, thickness, 255 );
			}
		}
	}
	catch(...)
	{
		if(release_ft_bmp) FT_Bitmap_Done(FTFace->glyph->library, ft_bmp);
		throw;
	}
	if(release_ft_bmp) FT_Bitmap_Done(FTFace->glyph->library, ft_bmp);

	return glyph_bmp;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
/**
 * �w�肵�������R�[�h�ɑ΂���`��̈�𓾂�
 * @param code	�����R�[�h
 * @return	�����_�����O�̈��`�ւ̃|�C���^
 *			NULL �̏ꍇ�͕ϊ��Ɏ��s�����ꍇ
 */
bool tFreeTypeFace::GetGlyphRectFromCharcode( tTVPRect& rt, tjs_char code, tjs_int& advancex, tjs_int& advancey )
{
	advancex = advancey = 0;
	if( !LoadGlyphSlotFromCharcode(code) )
		return false;

	int baseline = (int)( FTFace->ascender ) * FTFace->size->metrics.y_ppem / FTFace->units_per_EM;
	/*
	FT_Render_Glyph �Ń����_�����O���Ȃ��ƈȉ��̊e�l�͎擾�ł��Ȃ�
	tjs_int t = baseline - FTFace->glyph->bitmap_top;
	tjs_int l = FTFace->glyph->bitmap_left;
	tjs_int w = FTFace->glyph->bitmap.width;
	tjs_int h = FTFace->glyph->bitmap.rows;
	*/
	tjs_int t = baseline - FT_PosToInt( FTFace->glyph->metrics.horiBearingY );
	tjs_int l = FT_PosToInt( FTFace->glyph->metrics.horiBearingX );
	tjs_int w = FT_PosToInt( FTFace->glyph->metrics.width );
	tjs_int h = FT_PosToInt( FTFace->glyph->metrics.height );
	advancex = FT_PosToInt( FTFace->glyph->advance.x );
	advancey = FT_PosToInt( FTFace->glyph->advance.y );
	rt = tTVPRect(l,t,l+w,t+h);
	if( Options & TVP_TF_UNDERLINE ) {
		tjs_int pos = -1, thickness = -1;
		GetUnderline( pos, thickness );
		if( pos >= 0 && thickness > 0 ) {
			if( rt.left > 0 ) rt.left = 0;
			if( rt.right < advancex ) rt.right = advancex;
			if( pos < rt.top ) rt.top = pos;
			if( (pos+thickness) >= rt.bottom ) rt.bottom = pos+thickness+1;

		}
	}
	if( Options & TVP_TF_STRIKEOUT ) {
		tjs_int pos = -1, thickness = -1;
		GetStrikeOut( pos, thickness );
		if( pos >= 0 && thickness > 0 ) {
			if( rt.left > 0 ) rt.left = 0;
			if( rt.right < advancex ) rt.right = advancex;
			if( pos < rt.top ) rt.top = pos;
			if( (pos+thickness) >= rt.bottom ) rt.bottom = pos+thickness+1;
		}
	}
	return true;
}

//---------------------------------------------------------------------------
/**
 * �w�肵�������R�[�h�ɑ΂���O���t�̐��@�𓾂�(������i�߂邽�߂̃T�C�Y)
 * @param code		�����R�[�h
 * @param metrics	���@
 * @return	�����̏ꍇ�^�A���s�̏ꍇ�U
 */
bool tFreeTypeFace::GetGlyphMetricsFromCharcode(tjs_char code,
	tGlyphMetrics & metrics)
{
	if(!LoadGlyphSlotFromCharcode(code)) return false;

	// ���g���b�N�\���̂��쐬
	// CellIncX �� CellIncY �� �s�N�Z���l�� 64 �{���ꂽ�l�Ȃ̂Œ���
	// ����͂��Ƃ��� FreeType �̎d�l������ǂ��ARisa�ł������I�ɂ�
	// ���̐��x�� CellIncX �� CellIncY ������
	metrics.CellIncX =  FTFace->glyph->advance.x;
	metrics.CellIncY =  FTFace->glyph->advance.y;

	return true;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
/**
 * �w�肵�������R�[�h�ɑ΂���O���t�̃T�C�Y�𓾂�(�����̑傫��)
 * @param code		�����R�[�h
 * @param metrics	�T�C�Y
 * @return	�����̏ꍇ�^�A���s�̏ꍇ�U
 */
bool tFreeTypeFace::GetGlyphSizeFromCharcode(tjs_char code, tGlyphMetrics & metrics)
{
	if(!LoadGlyphSlotFromCharcode(code)) return false;

	// ���g���b�N�\���̂��쐬
	metrics.CellIncX = FT_PosToInt( FTFace->glyph->metrics.horiAdvance );
	metrics.CellIncY = FT_PosToInt( FTFace->glyph->metrics.vertAdvance );

	return true;
}

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
/**
 * �w�肵�������R�[�h�ɑ΂���O���t���O���t�X���b�g�ɐݒ肷��
 * @param code	�����R�[�h
 * @return	�����̏ꍇ�^�A���s�̏ꍇ�U
 */
bool tFreeTypeFace::LoadGlyphSlotFromCharcode(tjs_char code)
{
	// TODO: �X���b�h�ی�

	// �����R�[�h�𓾂�
	FT_ULong localcode;
	if(UnicodeToLocalChar == NULL)
		localcode = code;
	else
		localcode = UnicodeToLocalChar(code);

	// �����R�[�h���� index �𓾂�
	FT_UInt glyph_index = FT_Get_Char_Index(FTFace, localcode);
	if(glyph_index == 0)
		return false;

	// �O���t�X���b�g�ɕ�����ǂݍ���
	FT_Int32 load_glyph_flag = 0;
	if(!(Options & TVP_FACE_OPTIONS_NO_ANTIALIASING))
		load_glyph_flag |= FT_LOAD_NO_BITMAP;
	else
		load_glyph_flag |= FT_LOAD_TARGET_MONO;
			// note: �r�b�g�}�b�v�t�H���g��ǂݍ��݂����Ȃ��ꍇ�� FT_LOAD_NO_BITMAP ���w��

	if(Options & TVP_FACE_OPTIONS_NO_HINTING)
		load_glyph_flag |= FT_LOAD_NO_HINTING|FT_LOAD_NO_AUTOHINT;
	if(Options & TVP_FACE_OPTIONS_FORCE_AUTO_HINTING)
		load_glyph_flag |= FT_LOAD_FORCE_AUTOHINT;

	FT_Error err;
	err = FT_Load_Glyph(FTFace, glyph_index, load_glyph_flag);

	if(err) return false;

	// �t�H���g�̕ό`���s��
	if( Options & TVP_TF_BOLD ) FT_GlyphSlot_Embolden(FTFace->glyph);
	if( Options & TVP_TF_ITALIC ) FT_GlyphSlot_Oblique( FTFace->glyph );

	return true;
}
//---------------------------------------------------------------------------

float FT_fixed26p6_to_float(long fixP) {
	const unsigned long fractional_base = 1 << 6;
	const unsigned long fractional_mask = fractional_base - 1;
	return (float)(abs(fixP) & fractional_mask) / fractional_base + (fixP >> 6);
}

const FT_Outline* tFreeTypeFace::GetOulineData(tjs_char code, float &w, float &h)
{
	FT_UInt glyph_index = FT_Get_Char_Index(FTFace, code);
	if (glyph_index == 0) return GetOulineData(GetDefaultChar(), w, h);
	if (FT_Load_Glyph(FTFace, glyph_index, FT_LOAD_DEFAULT)) {
		return nullptr;
	}
	w = FT_fixed26p6_to_float(FTFace->glyph->advance.x);
	h = FT_fixed26p6_to_float(FTFace->glyph->advance.y);
	FT_GlyphSlot pGlyphSlot = FTFace->glyph;
	return &pGlyphSlot->outline;
}
