/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * text.h - the text engine
 * the glyph sheet and cursor pictures, the ruby (furigana) dictionary, the
 * message-window text output with its paced "rich" variant (horizontal and
 * vertical), and the immediate text layout used by Window_DrawText and the
 * text-to-bitmap instructions.  The implementation is spread over the
 * files of src/gfx/text (settings, glyphs, ruby, strings, markup, the
 * layouts) and src/wait/wait_text.c (the wait classes).
 *
 * Two layers:
 *
 *   * The plain output (WaitTextOut, src/wait/wait_text.c) draws one
 *     character per step straight into the window's text layer through
 *     the bitmap manager's glyph painter.
 *   * The rich output lays the whole string out first into a list of
 *     RichChar nodes (one per character, plus ruby glyphs spliced in after
 *     their base character), each holding a ready-made little bitmap with
 *     the glyph and its shadow.  The nodes are then revealed one by one with
 *     a per-character delay and fade (WaitTextRich), or blitted at once
 *     (Window_DrawText / Text_DrawToBitmap).  The layout handles word-level
 *     ruby from the dictionary, line-head prohibition of closing
 *     punctuation (kinsoku) with hanging into a reserved margin, "bracket
 *     hanging" of an opening bracket at the start of a paragraph, an
 *     optional underline mode, centred / right-aligned lines ("swing"), and
 *     the vertical writing direction with rotated and shifted glyphs.
 *
 * Japanese constants: the control bytes 4..8 that may start a string stand
 * for the opening brackets 「 (kagi), 　 (ideographic space), （, “ and 『
 * that are hung into the left margin; the character-class tables are the
 * closing punctuation that may not start a line (、。」etc.), the opening
 * brackets that may not end one, the glyphs rotated in vertical text, and
 * the small kana / punctuation shifted in vertical text.  The tables live
 * in tools/messages.txt as the MSG_TXT_* strings.
 */
#ifndef BGI_GFX_TEXT_H_
#define BGI_GFX_TEXT_H_

#include "bgi/gfx/font.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/gfx/window.h"
#include "bgi/wait.h"

// ---- the engine's own copies of the manager pointers --------------------------
extern Gfx_t* gTextGfx;
extern BmpMgr_t* gTextBmpMgr;
void Text_SetGfxPtr(Gfx_t* g); // both are set once by Engine_CreateCore
void Text_SetBmpMgrPtr(BmpMgr_t* m);

// ---- custom glyph sheet (codes 0xFF01 .. 0xFFFF) and the cursor pictures --
extern Bmp_t gGlyphSheet;      // a row of equal cells, one per private code
extern int32_t gGlyphCount;    // cells in the sheet (0: no sheet)
extern int32_t gGlyphCellW;    // width of one cell in pixels
extern int32_t gTextItemCount; // "waiting" cursor animation frames
extern Bmp_t* gTextItems;      // gTextItemCount screen bitmaps

int Text_SetGlyphSheet(int count, int bmp);                                // "90 9E": 0 / 0x8000000n
void Text_FreeGlyphSheet(void);                                            // (also drops the "92 98" pictures)
int Text_SetCharImage(uint32_t code, int bmp, int x, int y, int w, int h); // "92 98" (1.588 on): 0 / 0x8000000n
const Bmp_t* Text_CharImage(uint32_t code);                                // the picture of a private code, or NULL
// like Font_PaintGlyph, but codes 0xFF01.. come from the sheet or their picture
void Text_PaintGlyph(Bmp_t* cell, Glyph_t* g, int code, FontRaster_t* font, uint32_t colour);
int Text_SetItemBitmaps(int n, const int* bmpList, int* outBad); // "90 98": 1 ok, 0 and *outBad the bad bitmap
void Text_FreeItemBitmaps(void);

// ---- pacing and style of the plain output ------------------------------------
extern int32_t gCursorInterval;                // ms between cursor frames ("90 99")
extern int32_t gCharInterval;                  // ms between characters ("90 94")
extern int32_t gScrollSteps, gScrollInterval;  // a line scroll: steps and ms per step ("90 95")
extern int32_t gFadeSteps, gFadeInterval;      // a page-break fade: steps and ms per step ("90 96")
extern int32_t gTextStyle[5];                  // {shadowOn, dx%, dy%, shadowColour, density 0..0x100} ("90 9C" / "90 9D")
extern int32_t gCursorPosMode;                 // 1 = the cursor picture at the fixed position ("90 9A")
extern int32_t gCursorX, gCursorY;             // that position, in text-layer pixels
extern int32_t gStartDelayOn, gStartDelayMs;   // "90 9B": the end-of-text wait starts after `ms`
extern int32_t gAutoAdvanceOn, gAutoAdvanceMs; // "90 97": the end-of-text wait ends by itself after `ms`
extern int32_t gTextInputFinishes;             // "90 9F": input finishes the output instead of hurrying it
extern int32_t gTextLayerMode;                 // "90 91": 0 key 2, 1 fixed layer, 2 window priority
extern int32_t gTextLayerNo;                   // the fixed layer of mode 1 (< 0x1000)

void Text_SetCursorInterval(int ms);
void Text_SetCursorPos(int mode, int x, int y);
void Text_SetStartDelay(int on, int ms);
void Text_SetCharInterval(int ms);
int Text_SetScroll(int steps, int ms); // 1 when steps > 0 (stored), else 0
int Text_SetFade(int steps, int ms);   // 1 when steps > 0 (stored), else 0
void Text_SetAutoAdvance(int on, int ms);
void Text_SetStyleOn(int on);
int Text_SetStyleParams(int dxPct, int dyPct, int level); // 1 ok, 0 out of range (nothing stored)
void Text_GetStyle(int32_t out[5]);
// build a style block; on == 0 clears it. 1 ok, 0 bad range
int Text_MakeStyle(int32_t out[5], int on, int dxPct, int dyPct, uint32_t shadowColour, int level);
void Text_SetInputFinishes(int f);
int Text_SetLayerMode(int mode, int layerNo); // 0 ok / 0x80000004 bad mode / 0x80000005 bad layer

// ---- ruby -------------------------------------------------------------------
typedef struct RubyNode // one dictionary entry; the list head is a node too
{
	char* word;            // the base text, Shift-JIS
	int32_t wordLen;       // strlen + 1
	int32_t wordChars;     // characters in `word`
	char* reading;         // the reading, Shift-JIS
	int32_t readingLen;    // strlen + 1
	int32_t* readingCodes; // one Shift-JIS code per character of the reading
	int32_t readingChars;  // characters in `reading`
	struct RubyNode* next;
} RubyNode_t;
typedef RubyNode_t RubyDict_t; // the list head (its own fields unused)

extern RubyDict_t gRubyDict;   // the script's dictionary ("91 94" / "91 96")
extern int32_t gRubyCharDelay; // reveal delay added per character of the rich output (steps)
extern int32_t gRubyFadeSteps; // steps of a character's fade-in
extern int32_t gRubySizePct;   // ruby size in percent of the font
// 1.529 on ("91 97"): the ruby font override, see Text_SetRubyFont
extern char gRubyFace[0x100];                                   // empty = the base font's face
extern int32_t gRubySizeFixed, gRubyWidthPct, gRubyDx, gRubyDy; // size and width (0 = derived), offset in pixels
extern int32_t gRubyBold, gRubyShadow;                          // "92 97" (1.616 on): -1 = the base font's bold / the style's shadow colour
void Text_SetRubyFont(const char* face, int size, int widthPct, int dx, int dy);

/* "91 9A" (1.573 on): the text functions, numbered 0 and 0x80000000 ..
 * 0x8000000A; each build accepts the ones it had (Text_SetFunction: 0 ok,
 * 0x80000007 unknown number, 0x80000008 bad parameter).  Known: function
 * 0 (default 1) gates the rule that lets the closing punctuation after a
 * character stay on its line past the margin; the others are stored. */
#define TEXT_FUNC_MAX 12
extern int32_t gTextFunc[TEXT_FUNC_MAX]; // [0] = function 0, [1 + n] = function 0x80000000 + n
uint32_t Text_SetFunction(uint32_t func, int32_t param);
int Text_SetScaleDivisor(int32_t v); // "91 99" (1.529 on): 1 ok, 0 bad
extern int32_t gCharPitch;           // extra pixels per non-proportional character
extern int32_t gHangBrackets;        // hang a leading bracket into the margin
extern int32_t gTextIndent;          // line-start indent when ruby is on (the margin "91 98" leaves for readings), pixels

// "91 98": the parameters of the rich output; 0 ok, 0x80000001 bad sizePct, 0x80000002 bad indent
int Text_SetRichParams(int charDelay, int fadeSteps, int pitch, int sizePct, int indent, int hangBrackets);
int32_t Text_GetIndent(void);
void RubyDict_Insert(RubyDict_t* d, const char* word, const char* reading);
int RubyDict_Remove(RubyDict_t* d, const char* word); // 1 removed
void RubyDict_Clear(RubyDict_t* d);
int RubyDict_Find(RubyNode_t* out, const char* word, RubyDict_t* d);      // 1 found
int RubyDict_MatchPrefix(char* outWord, const char* text, RubyDict_t* d); // 1 when a word starts at `text`
int RubyDict_ParseList(RubyDict_t* d, const char* list);                  // "word\reading\n..."; 1 when all consumed
void Ruby_Add(const char* word, const char* reading);                     // the same on gRubyDict
int Ruby_AddList(const char* list);
int Ruby_Remove(const char* word);
void Ruby_Clear(void);
int SjisCodes(int32_t* outOrNull, const char* s); // character count
/* find the ruby words in `str` and write "word\reading\n" lines
 * to `tags` (0x400 bytes); returns the number of words ("91 95") */
int Text_ParseTags(char* tags, const char* str);

// ---- the rich layout ------------------------------------------------------------
typedef struct RichChar // one laid-out glyph
{
	int32_t shown;      // fully drawn (the paced output's bookkeeping)
	int32_t delay;      // steps before the fade-in starts
	int32_t fadeStep;   // current step of the fade-in
	int32_t fadeSteps;  // steps the fade-in takes (gRubyFadeSteps)
	int32_t x, y;       // in the text layer, pixels
	Bmp_t glyph;        // the glyph with its shadow
	char* rubyWord;     // set on the base character of a ruby word: the word
	int32_t rubyExtent; // width (height) the reading needs, pixels
	int32_t kind;       // 0 normal, 1 shifted small (vertical), 2 ruby
	struct RichChar* next;
} RichChar_t;

typedef struct RichState // the output of a layout
{
	int32_t unused[15]; // unused; keeps the layout of the original's record
	RichChar_t* head;   // the nodes in reading order
} RichState_t;

void RichState_Free(RichState_t* r);
int FontInfo_GlyphW(const FontInfo_t* fi); // size * widthPct / 100
int Text_RubySize(int size);               // max(4, size * pct / 100)
// width of `str` in `font`; out = {total, total - firstGap/2 - lastGap/2, total - lastGap/2}
int Text_Measure(int32_t out[3], const char* str, int font, int proportional);
// the same with the font given (the markup's temporary fonts)
int Text_MeasureInfo(int32_t out[3], const char* str, const FontInfo_t* fi, int proportional);

/* ---- the inline markup of 1.494 on (horizontal text) -------------------------
 * "<l>" sections are clickable links: each layout registers the text and
 * the position of its first glyph in a table of TEXT_LINKS_MAX entries of
 * 0x80 bytes; "92 9E" takes them.  "92 9D" / "92 9F" set the font and
 * colour they are drawn in. */
#define TEXT_LINKS_MAX 16
typedef struct TextLink
{
	char text[0x78]; // the link's text (the original copies up to 0x5F bytes)
	int32_t x, y;    // the first glyph's position in the text layer
} TextLink_t;
extern TextLink_t gTextLinks[TEXT_LINKS_MAX];
extern int32_t gTextLinkCount;
void TextLinks_Clear(void);
int TextLinks_Take(TextLink_t* out);                                                  // copy out and clear; the count
extern char gLinkFace[0x34];                                                          // empty = the text's face
extern int32_t gLinkSize, gLinkWidthPct, gLinkBold, gLinkItalic;                      // 0 = the text's
extern int32_t gLinkColour;                                                           // -1 = the text's
int Text_SetLinkFont(const char* face, int size, int widthPct, int bold, int italic); // "92 9D": 0 / 0x8000000n
void Text_SetLinkColour(int32_t colour);                                              // "92 9F"
int Text_CountLinks(TextLink_t* outOrNull, const char* str);                          // "91 9E": the "<l>..</l>" sections of a string
int Text_StripTags(char* out, const char* str);                                       // "91 9F": copy without the markup; the length
int Text_CollectClosing(char* out, const char* p);                                    // the run of closing punctuation at `p`; its count
int Text_IsHangBracket(const char* p);                                                // first character is a bracket that may hang
int Text_IsClosing(const char* p);                                                    // first character may overhang the right margin
int Text_IsOpening(const char* p);                                                    // first character may not end a line
int Text_NthChar(char* out, const char* s, int n);                                    // the n-th character of `s`; 1 when it exists
int Text_VertClass(int32_t out[3], const char* s);                                    // {rotate, dx%, dy%} in vertical text
int Bmp_RotateSquare(Bmp_t* b);                                                       // 90 degrees clockwise; 0 if not square

/* the layout cores (horizontal, vertical): one RichChar node per glyph of
 * `str` from the cursor `cur` inside `area`, lines `adv` pixels apart, in
 * `font` and `colour` with the shadow `style`; `rubyOn` reserves room for
 * readings from `dict`, `hang` enables the kinsoku rules.  *outLines gets
 * the line count, `cur` the cursor after the text.  0 when the font is
 * unknown. */
int Text_LayoutCoreH(RichState_t* rich, int32_t* outLines, const char* str, int rubyOn, RubyDict_t* dict,
	int32_t cur[2], const Rect_t* area, int adv, int font, int proportional,
	int hang, uint32_t colour, const int32_t style[5]);
int Text_LayoutCoreV(RichState_t* rich, int32_t* outLines, const char* str, int rubyOn, RubyDict_t* dict,
	int32_t cur[2], const Rect_t* area, int adv, int font, int proportional,
	int hang, uint32_t colour, const int32_t style[5]);
// add the ruby readings of the laid-out words; 0 when the ruby font cannot be opened
int Text_RubyPlaceH(RichState_t* rich, int font, uint32_t colour, const int32_t style[5], RubyDict_t* dict);
int Text_RubyPlaceV(RichState_t* rich, int font, uint32_t colour, const int32_t style[5], RubyDict_t* dict);
// shift whole lines for `swing` 1 (centred) or 2 (right-aligned); 0 leaves them
void Text_AlignH(RichState_t* rich, int32_t cur[2], const Rect_t* area, int font, int hang,
	const int32_t style[5], int swing);
void Text_AlignV(RichState_t* rich, int32_t cur[2], const Rect_t* area, int font, int hang,
	const int32_t style[5], int swing);
// blit every node into `dst`, one rectangle per node into `rects`; the node count
int Text_Collect(Bmp_t* dst, Rect_t* rects, RichState_t* rich, int effect, int level);

// lay out and blit at once (Window_DrawText); 1 ok, 0 unknown font
int Text_LayoutH(Bmp_t* text, int* outRectCount, Rect_t* rects, int* outLines, int32_t cur[2],
	const Rect_t* area, const char* str, int parseTags, const char* tags, int font,
	int proportional, int hang, int swing, int spacing, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5], int effect, int level);
int Text_LayoutV(Bmp_t* text, int* outRectCount, Rect_t* rects, int* outLines, int32_t cur[2],
	const Rect_t* area, const char* str, int parseTags, const char* tags, int font,
	int proportional, int hang, int swing, int spacing, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5], int effect, int level);
// the horizontal layout into a whole bitmap ("91 9C" / "91 9D" / "92 9C")
int Text_DrawToBitmap(Bmp_t* dst, int* outLines, int x, int y, const char* str, int parseTags,
	const char* tags, int font, int proportional, int hang, int swing, int spacing,
	uint32_t colour, uint32_t rubyColour, const int32_t style[5], int effect, int level);

// ---- the text wait classes (src/wait/wait_text.c) -------------------------------
typedef struct WaitTextOut WaitTextOut_t;

typedef struct WaitTextVtbl // the plain class fills the first 11 slots, the rich classes all 14
{
	WaitVtbl_t base;
	void (*setText)(WaitTextOut_t* w, const char* p);       // the string to output (not copied)
	void (*setParam)(WaitTextOut_t* w, uint32_t v);         // the text colour
	int (*step)(WaitTextOut_t* w);                          // 1 when the text is done
	int (*pageBreak)(WaitTextOut_t* w);                     // bytes consumed
	void (*cursorNewLine)(WaitTextOut_t* w);                // after a new line made for the cursor picture
	void (*cursorOffset)(WaitTextOut_t* w, int32_t out[2]); // the cursor picture's offset from the text cursor
	// rich only
	int (*layoutCore)(WaitTextOut_t* w, RichState_t* rich, int32_t* outLines, const char* str, int rubyOn,
		RubyDict_t* dict, int32_t cur[2], const Rect_t* area, int adv, int font, int proportional,
		int hang, uint32_t colour, const int32_t style[5]);
	int (*rubyPlace)(WaitTextOut_t* w, RichState_t* rich, int font, uint32_t colour,
		const int32_t style[5], RubyDict_t* dict);
	void (*align)(WaitTextOut_t* w, RichState_t* rich, int32_t cur[2], const Rect_t* area, int font, int hang,
		const int32_t style[5], int swing);
} WaitTextVtbl_t;

struct WaitTextOut
{ // the plain output ("90 90")
	Wait_t w;
	Window_t* win;            // the window written to
	uint32_t input;           // masked Input_Poll result of this poll
	const char* text;         // read cursor
	uint32_t param;           // the text colour
	int32_t instant;          // no pacing between characters
	int32_t fast;             // input seen: stop pacing (the result)
	int32_t waitAtEnd;        // show the cursor animation and wait for input at the end
	int32_t allowSkip;        // the skip bit of the input counts
	int32_t allowKeys;        // the down key and wheel down count
	int32_t startDelayOn;     // the end-of-text wait is still held back ("90 9B")
	uint32_t startDelayUntil; // tick count the hold ends at
	int32_t scrollSteps;      // the running line scroll: steps and ms per step
	int32_t scrollInterval;
	int32_t fadeSteps; // the running page-break fade: steps and ms per step
	int32_t fadeInterval;
	uint32_t autoAdvanceAt; // tick count the auto-advance fires at (0: none)
	int32_t scrolling;      // a line scroll is in progress
	int32_t phase;          // sub-step of a scroll, fade, character or cursor animation
	int32_t overhang;       // a kinsoku character was allowed past the margin
	int32_t clearing;       // control byte 1 is scrolling the text out
	int32_t linesLeft;      // lines still to scroll away in that clear
	int32_t finishNow;      // end the wait at the next step
	uint32_t layerKey;      // the input layer key pushed for the output
};

typedef struct WaitTextRich // the paced rich output ("91 90", "91 92", "92 90")
{
	WaitTextOut_t t;
	RichState_t rich;    // the laid-out string
	int32_t rubyOn;      // readings are shown
	int32_t style[5];    // the shadow style the glyphs were built with
	uint32_t rubyColour; // the readings' colour
} WaitTextRich_t;

extern const WaitTextVtbl_t WaitTextOut_Vtbl, WaitTextRich_Vtbl, WaitTextRichV_Vtbl;

WaitTextOut_t* WaitTextOut_New(Thread_t* t, Window_t* win);
WaitTextRich_t* WaitTextRich_New(Thread_t* t, Window_t* win);
WaitTextRich_t* WaitTextRichV_New(Thread_t* t, Window_t* win);
void WaitTextOut_SetInstant(WaitTextOut_t* w, int f);
void WaitTextOut_SetWaitAtEnd(WaitTextOut_t* w, int f);
void WaitTextOut_SetAllowSkip(WaitTextOut_t* w, int f);
void WaitTextOut_SetAllowKeys(WaitTextOut_t* w, int f);
void WaitTextOut_SetFast(WaitTextOut_t* w, int f);
void WaitTextOut_FinishNow(WaitTextOut_t* w);
// the steps of the plain output (also used by the rich classes)
int WaitTextOut_CtlClear(WaitTextOut_t* w);   // bytes consumed
int WaitTextOut_CtlNewLine(WaitTextOut_t* w); // bytes consumed
void WaitTextOut_ScrollStep(WaitTextOut_t* w);
int WaitTextOut_DrawChar(WaitTextOut_t* w, const char* p); // bytes consumed
int WaitTextOut_CursorAnim(WaitTextOut_t* w);              // 1 on input
int WaitTextOut_EndOfText(WaitTextOut_t* w);               // 1 when the wait is over
// lay the string out (rich classes); 1 ok
int WaitTextRich_Prepare(WaitTextRich_t* w, const char* str, int rubyOn, int hang, int plainStyle);
// drop the current layout and lay `str` out
int WaitTextRich_SetTextEx(WaitTextRich_t* w, const char* str, int rubyOn, int hang, int plainStyle);
void WaitTextRich_SetColours(WaitTextRich_t* w, uint32_t colour, uint32_t rubyColour);
void WaitTextRich_SetRubyColour(WaitTextRich_t* w, uint32_t colour);
void WaitTextRich_SetStyle(WaitTextRich_t* w, const int32_t s[5]);

#endif // BGI_GFX_TEXT_H_
