/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * font.c - the Apple back end's glyph rasteriser (inc/bgi/os.h, "Fonts")
 *          through Core Text
 *
 * GDI's cell is reproduced as the POSIX back end does it with FreeType: the
 * font is scaled so that usWinAscent + usWinDescent of the OS/2 table fill
 * the requested height, the baseline sits at the scaled ascent, and a
 * full-width glyph ("あ") is stretched to twice the requested width.  The
 * coverage is drawn antialiased into the engine's 8-bit buffer; the engine
 * oversamples and filters it itself (src/gfx/font.c).
 *
 * The games ask for the Windows faces ＭＳ ゴシック / ＭＳ 明朝, which these
 * systems do not have: a Mincho name maps to Hiragino Mincho, anything else
 * to Hiragino Sans (W6 for bold), with the system's Japanese font as the
 * last resort.
 *
 * Under the Chinese patch (gAppleChinese) a two-byte character is GBK, not
 * Shift-JIS: the patch keeps the engine's Shift-JIS character boundaries
 * by spelling the common hanzi whose GBK lead byte falls in 0xA1 .. 0xDF
 * with rare GBK characters instead, which its font.ttf draws as the
 * intended ones.  Each character is therefore decoded as GBK and looked up
 * in that font first (registered by AppleFont_Register), then in the
 * system's simplified Chinese font.
 */
#include "apple_internal.h"
#include "bgi/msg.h"

#include <strings.h>
#include <unistd.h>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

struct OsFont
{
	CTFontRef primary;  // the face asked for (or its stand-in)
	CTFontRef fallback; // where a character the primary lacks is looked up (NULL: Core Text's cascade)
	int height, width;  // as requested, pixels
	int ascent;         // the baseline row inside the cell
	CGFloat xscale;     // horizontal stretch of the glyphs (lfWidth)
	int fakeBold;       // the face has no bold weight: overstrike by one pixel, as GDI does
	int italic;         // shear the glyphs
};

static char gPatchFace[256]; // the PostScript name of the registered patch font, "" for none

// ---- text decoding --------------------------------------------------------------------------

static uint16_t* gGbk; // GBK code (lead - 0x80) << 8 | trail -> UTF-16, computed on demand; 0 = not yet

// one two-byte GBK code as a code point; U+FFFD when it has none
static uint32_t GbkChar(uint16_t code)
{
	uint32_t idx;
	if(code < 0x8000)
		return 0xfffd;
	idx = code - 0x8000u;
	if(!gGbk)
	{
		gGbk = (uint16_t*)calloc(0x8000, sizeof *gGbk);
		if(!gGbk)
			return 0xfffd;
	}
	if(!gGbk[idx])
	{
		uint8_t b[2] = {(uint8_t)(code >> 8), (uint8_t)code};
		CFStringRef s = CFStringCreateWithBytes(NULL, b, 2, kCFStringEncodingGB_18030_2000, false);
		UniChar u = 0xfffd;
		if(s && CFStringGetLength(s) == 1)
			u = CFStringGetCharacterAtIndex(s, 0);
		if(s)
			CFRelease(s);
		gGbk[idx] = u ? u : 0xfffd;
	}
	return gGbk[idx];
}

// a two-byte code of the text encoding: GBK under the Chinese patch, else Shift-JIS
static uint32_t TwoByteChar(uint16_t code)
{
	return gAppleChinese ? GbkChar(code) : OsPosix_SjisChar(code);
}

// the code point of a character of `len` bytes (OsCommon_CodePoint handles UTF-8 text and single bytes)
static uint32_t CodePoint(const char* s, int len)
{
	return OsCommon_CodePoint(s, len, TwoByteChar);
}

/* Text the engine hands the application (message boxes) as UTF-8: GBK or
 * Shift-JIS by the same rule, on the Shift-JIS character boundaries the
 * engine uses */
void AppleText_ToUtf8(const char* text, char* out, size_t n)
{
	OsCommon_SjisToUtf8(text ? text : "", out, n, TwoByteChar);
}

// ---- faces ----------------------------------------------------------------------------------------

static int ContainsNoCase(const char* s, const char* sub)
{
	size_t n = strlen(sub);
	for(; *s; s++)
		if(strncasecmp(s, sub, n) == 0)
			return 1;
	return 0;
}

// a font of that PostScript name at `size`, NULL when the system substituted another one
static CTFontRef CreateNamed(const char* psName, CGFloat size)
{
	CFStringRef name = CFStringCreateWithCString(NULL, psName, kCFStringEncodingUTF8);
	CTFontRef f;
	CFStringRef got;
	if(!name)
		return NULL;
	f = CTFontCreateWithName(name, size, NULL);
	got = f ? CTFontCopyPostScriptName(f) : NULL;
	if(!got || CFStringCompare(got, name, kCFCompareCaseInsensitive) != kCFCompareEqualTo)
	{
		if(f)
			CFRelease(f);
		f = NULL;
	}
	if(got)
		CFRelease(got);
	CFRelease(name);
	return f;
}

// the system's UI font for a language ("ja", "zh-Hans") at `size`
static CTFontRef CreateForLanguage(const char* lang, CGFloat size)
{
	CFStringRef l = CFStringCreateWithCString(NULL, lang, kCFStringEncodingUTF8);
	CTFontRef f = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, l);
	if(l)
		CFRelease(l);
	return f;
}

/* The face for a Shift-JIS face name: a Japanese stand-in for the Windows
 * faces.  *realBold says whether the result is a bold weight itself. */
static CTFontRef CreateJapanese(const char* sjisFace, int bold, CGFloat size, int* realBold)
{
	char utf[512];
	CTFontRef f = NULL;
	int mincho;
	OsPosix_SjisToUtf8(sjisFace ? sjisFace : "", utf, sizeof utf);
	mincho = strstr(utf, "明朝") != NULL || ContainsNoCase(utf, "mincho") || ContainsNoCase(utf, "serif");
	*realBold = 0;
	if(mincho)
	{
		if(bold && (f = CreateNamed("HiraMinProN-W6", size)) != NULL)
			*realBold = 1;
		if(!f)
			f = CreateNamed("HiraMinProN-W3", size);
	}
	if(!f)
	{
		if(bold && (f = CreateNamed("HiraginoSans-W6", size)) != NULL)
			*realBold = 1;
		if(!f)
			f = CreateNamed("HiraginoSans-W3", size);
	}
	if(!f)
		f = CreateForLanguage("ja", size);
	return f;
}

// the simplified Chinese stand-in for the characters the patch font lacks
static CTFontRef CreateChinese(int bold, CGFloat size, int* realBold)
{
	CTFontRef f = NULL;
	*realBold = 0;
	if(bold && (f = CreateNamed("PingFangSC-Semibold", size)) != NULL)
		*realBold = 1;
	if(!f)
		f = CreateNamed("PingFangSC-Regular", size);
	if(!f)
		f = CreateForLanguage("zh-Hans", size);
	return f;
}

/* GDI's cell of a face, in font units: usWinAscent and usWinDescent of the
 * OS/2 table, Core Text's ascent and descent when there is none */
static void CellMetrics(CTFontRef f, double* asc, double* desc, double* upem)
{
	CFDataRef os2 = CTFontCopyTable(f, kCTFontTableOS2, kCTFontTableOptionNoOptions);
	CGFloat size = CTFontGetSize(f);
	*upem = CTFontGetUnitsPerEm(f);
	*asc = *desc = 0;
	if(os2 && CFDataGetLength(os2) >= 78)
	{
		const uint8_t* t = CFDataGetBytePtr(os2);
		*asc = t[74] << 8 | t[75];
		*desc = t[76] << 8 | t[77];
	}
	if(os2)
		CFRelease(os2);
	if(*asc + *desc <= 0 && size > 0)
	{
		*asc = CTFontGetAscent(f) * *upem / size;
		*desc = CTFontGetDescent(f) * *upem / size;
	}
	if(*asc + *desc <= 0)
	{
		*asc = *upem;
		*desc = 0;
	}
}

// the advance of one character in a font, 0 when the font has no glyph for it
static CGFloat Advance(CTFontRef f, UniChar c)
{
	CGGlyph g = 0;
	CGSize adv;
	if(!CTFontGetGlyphsForCharacters(f, &c, &g, 1) || !g)
		return 0;
	CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, &g, &adv, 1);
	return adv.width;
}

OsFont_t* OS_FontOpen(const char* face, int height, int width, int bold, int italic)
{
	OsFont_t* f;
	CTFontRef probe;
	double asc, desc, upem, cell;
	CGFloat size, full;
	int realBold = 0, fbBold = 0;
	if(height <= 0)
		return NULL;
	f = (OsFont_t*)calloc(1, sizeof *f);
	if(!f)
		return NULL;
	// measure the cell at a reference size, then open the face at the size that makes it `height`
	if(gAppleChinese && gPatchFace[0])
		probe = CreateNamed(gPatchFace, 100);
	else if(gAppleChinese)
		probe = CreateChinese(bold, 100, &realBold);
	else
		probe = CreateJapanese(face, bold, 100, &realBold);
	if(!probe)
	{
		free(f);
		return NULL;
	}
	CellMetrics(probe, &asc, &desc, &upem);
	cell = asc + desc;
	size = (CGFloat)(height * upem / cell);
	f->primary = CTFontCreateCopyWithAttributes(probe, size, NULL, NULL);
	CFRelease(probe);
	if(gAppleChinese)
		f->fallback = CreateChinese(bold, size, &fbBold);
	f->height = height;
	f->width = width;
	f->ascent = (int)(height * asc / cell + 0.5);
	f->italic = italic;
	f->fakeBold = bold && !(gAppleChinese && !gPatchFace[0] ? fbBold : realBold);
	// lfWidth: a full-width glyph is 2 * width pixels; a face without "あ" is taken to be an em wide
	full = Advance(f->primary, 0x3042);
	if(full <= 0 && f->fallback)
		full = Advance(f->fallback, 0x3042);
	if(full <= 0)
		full = size;
	f->xscale = width > 0 ? (CGFloat)(2 * width) / full : 1;
	return f;
}

OsFont_t* OS_FontOpenMono(const char* face, int size, int bold)
{
	return OS_FontOpen(face, size, size / 2, bold, 0);
}

void OS_FontClose(OsFont_t* f)
{
	if(!f)
		return;
	if(f->primary)
		CFRelease(f->primary);
	if(f->fallback)
		CFRelease(f->fallback);
	free(f);
}

void OsPosix_FontShutdown(void)
{
	free(gGbk);
	gGbk = NULL;
}

// ---- drawing ----------------------------------------------------------------------------------------

/* the glyph of a code point and the font that has it: the primary face,
 * the fallback, Core Text's cascade; "□" when nobody has the character, as
 * GDI shows a box.  The returned font is retained. */
static CGGlyph FindGlyph(const OsFont_t* f, uint32_t cp, CTFontRef* font)
{
	UniChar u[2];
	CGGlyph g[2] = {0, 0};
	CFIndex n;
	if(cp >= 0x10000)
	{
		u[0] = (UniChar)(0xd800 + ((cp - 0x10000) >> 10));
		u[1] = (UniChar)(0xdc00 + ((cp - 0x10000) & 0x3ff));
		n = 2;
	}
	else
	{
		u[0] = (UniChar)cp;
		n = 1;
	}
	if(CTFontGetGlyphsForCharacters(f->primary, u, g, n) && g[0])
	{
		*font = (CTFontRef)CFRetain(f->primary);
		return g[0];
	}
	if(f->fallback && CTFontGetGlyphsForCharacters(f->fallback, u, g, n) && g[0])
	{
		*font = (CTFontRef)CFRetain(f->fallback);
		return g[0];
	}
	{
		CFStringRef s = CFStringCreateWithCharacters(NULL, u, n);
		CTFontRef sub = s ? CTFontCreateForString(f->primary, s, CFRangeMake(0, n)) : NULL;
		if(s)
			CFRelease(s);
		if(sub && CTFontGetGlyphsForCharacters(sub, u, g, n) && g[0])
		{
			*font = sub;
			return g[0];
		}
		if(sub)
			CFRelease(sub);
	}
	if(cp != 0x25a1 && cp != 0x20)
		return FindGlyph(f, 0x25a1, font);
	*font = NULL;
	return 0;
}

/* Draw the character into the w x h 8-bit buffer (cleared first): ink as
 * coverage 0 .. 255, the cell's top-left at the buffer's top-left. */
static void Draw(OsFont_t* f, const char* s, int len, uint8_t* buf, int pitch, int w, int h)
{
	CGColorSpaceRef gray;
	CGContextRef ctx;
	CTFontRef font = NULL;
	CGGlyph g;
	CGPoint pos;
	CGAffineTransform m;
	int r;
	uint32_t cp;
	for(r = 0; r < h; r++)
		memset(buf + (size_t)r * pitch, 0, (size_t)w);
	if(!f || w <= 0 || h <= 0)
		return;
	cp = CodePoint(s, len);
	if(cp == 0x20 || cp == 0x3000)
		return;
	g = FindGlyph(f, cp, &font);
	if(!font)
		return;
	gray = CGColorSpaceCreateDeviceGray();
	ctx = CGBitmapContextCreate(buf, (size_t)w, (size_t)h, 8, (size_t)pitch, gray, kCGImageAlphaNone);
	CGColorSpaceRelease(gray);
	if(!ctx)
	{
		CFRelease(font);
		return;
	}
	CGContextSetShouldAntialias(ctx, true);
	CGContextSetShouldSmoothFonts(ctx, false);
	CGContextSetGrayFillColor(ctx, 1.0, 1.0);
	CGContextSetGrayStrokeColor(ctx, 1.0, 1.0);
	// Core Graphics counts rows from the bottom: the baseline is `ascent` rows below the top
	m = CGAffineTransformMake(f->xscale, 0, f->italic ? 0.21 : 0, 1, 0, 0);
	CGContextSetTextMatrix(ctx, m);
	pos = CGPointMake(0, h - f->ascent);
	CTFontDrawGlyphs(font, &g, &pos, 1, ctx);
	if(f->fakeBold)
	{ // GDI's overstrike: the glyph again one pixel to the right
		pos.x += 1;
		CTFontDrawGlyphs(font, &g, &pos, 1, ctx);
	}
	CGContextRelease(ctx);
	CFRelease(font);
}

void OS_FontRender(OsFont_t* f, const char* sjis, int len, uint8_t* buf, int pitch, int w, int h)
{
	Draw(f, sjis, len, buf, pitch, w, h);
}

void OS_FontRenderMono(OsFont_t* f, const char* sjis, int len, uint8_t* bits, int pitch, int w, int h)
{
	uint8_t* gray = (uint8_t*)malloc((size_t)w * h);
	int x, y;
	for(y = 0; y < h; y++)
		memset(bits + (size_t)y * pitch, 0, (size_t)(w + 7) / 8);
	if(!gray)
		return;
	Draw(f, sjis, len, gray, w, w, h);
	for(y = 0; y < h; y++)
		for(x = 0; x < w; x++)
			if(gray[(size_t)y * w + x] >= 0x80)
				bits[(size_t)y * pitch + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
	free(gray);
}

int OS_FontCharAdvance(OsFont_t* f, const char* sjis, int len)
{
	CTFontRef font = NULL;
	CGGlyph g;
	CGSize adv;
	if(!f)
		return 0;
	g = FindGlyph(f, CodePoint(sjis, len), &font);
	if(!font)
		return f->width;
	CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &g, &adv, 1);
	CFRelease(font);
	return (int)(adv.width * f->xscale + 0.5);
}

// ---- font files and faces ------------------------------------------------------------------------------

// register a font file for this process; the first one registered is the patch font of the Chinese text
int AppleFont_Register(const char* path)
{
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
	CFArrayRef descs;
	int ok;
	if(!url)
		return 0;
	ok = CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, NULL) ? 1 : 0;
	descs = CTFontManagerCreateFontDescriptorsFromURL(url);
	if(descs && CFArrayGetCount(descs) > 0 && !gPatchFace[0])
	{
		CTFontDescriptorRef d = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descs, 0);
		CFStringRef name = (CFStringRef)CTFontDescriptorCopyAttribute(d, kCTFontNameAttribute);
		if(name)
		{
			CFStringGetCString(name, gPatchFace, sizeof gPatchFace, kCFStringEncodingUTF8);
			CFRelease(name);
		}
		ok = 1; // already registered by an earlier run counts as available
	}
	if(descs)
		CFRelease(descs);
	CFRelease(url);
	return ok;
}

// "B0 C2": a font file of the game (an engine path) for this process
int OS_FontAddFile(const char* path)
{
	char native[1024];
	CFURLRef url;
	int ok;
	OsPosix_Path(path, native, sizeof native);
	url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)native, (CFIndex)strlen(native), false);
	if(!url)
		return 0;
	ok = CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, NULL) ? 1 : 0;
	CFRelease(url);
	return ok;
}

// "B0 C3": a font file read from an archive, registered through a file in the temporary directory
int OS_FontAddMemory(const void* data, uint32_t size)
{
	static int count;
	char path[1024];
	const char* tmp = getenv("TMPDIR");
	FILE* f;
	CFURLRef url;
	int ok;
	snprintf(path, sizeof path, "%s/bgi-font-%d-%d.ttf", tmp ? tmp : "/tmp", (int)getpid(), count++);
	f = fopen(path, "wb");
	if(!f)
		return 0;
	ok = fwrite(data, 1, size, f) == size;
	if(fclose(f) != 0 || !ok)
		return 0;
	url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
	if(!url)
		return 0;
	ok = CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess, NULL) ? 1 : 0;
	CFRelease(url);
	return ok;
}

// every face is "installed": the Windows faces have their stand-ins above
int OS_FontFaceExists(const char* face)
{
	return face && face[0];
}

/* "B0 C4" / "B0 C5": the faces of a charset - the two Windows faces the
 * games expect, which OS_FontOpen maps to the system's */
int OS_FontEnumFaces(char* buf, int charset, int* outBytes)
{
	static const char* const kFaces[] = {MSG_FONT_GOTHIC, MSG_FONT_MINCHO};
	int i, bytes = 0;
	BGI_UNUSED(charset);
	for(i = 0; i < 2; i++)
	{
		size_t n = strlen(kFaces[i]) + 1;
		if(buf)
			memcpy(buf + bytes, kFaces[i], n);
		bytes += (int)n;
	}
	if(outBytes)
		*outBytes = bytes;
	return 2;
}
