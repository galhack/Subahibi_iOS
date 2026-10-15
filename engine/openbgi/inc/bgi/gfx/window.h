/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.h - the message Window display object (class order 3), the
 * target of the "90 8x" / "91 8x" / "92 8x" window instructions and of
 * every text output.  The implementation is the files of src/gfx/window.
 *
 * Unlike a sprite the window owns its pixels: three window-sized layers
 * (frame, text, composite), up to eight small item layers and optionally a
 * set of projected sub-sprites with a private compositor.  Whenever one of
 * the inputs changes the affected rectangle of the *composite* is rebuilt
 * (Window_Recomposite) and draw() simply blits the composite:
 *
 *   frame layer  --copy-->+
 *   text layer   --blend->+--> composite --effect/level--> screen
 *   item layers  --blend->|
 *   sub-sprites  --draw-->+
 *
 * The window also keeps the text cursor, font and layout style the text
 * engine (text.h) works with; the glyph rendering itself lives there.
 */
#ifndef BGI_GFX_WINDOW_H_
#define BGI_GFX_WINDOW_H_

#include "bgi/gfx/dispobj.h"
#include "bgi/gfx/compositor.h"

#define WIN_ITEMS 8 // item layers per window

typedef struct WinItem // one item layer: a small picture composited over the text
{
	int32_t enabled; // takes part in the composite
	PixBuf_t* buf;   // the pixel holder of `bmp`
	Bmp_t bmp;       // private copy of the item picture (pixels NULL: none yet)
	int32_t x, y;    // position inside the window, pixels
	int32_t level;   // transparency used when compositing (0 opaque .. 0x100)
} WinItem_t;

typedef struct Window
{
	DispObj_t obj;
	int32_t w, h;         // the layers' size in pixels
	int32_t ready;        // surfaces exist (createSurfaces result)
	PixBuf_t* compBuf;    // the pixel holders of the three layers
	Bmp_t comp;           // what draw shows
	int32_t frameVisible; // the frame layer takes part in the composite
	PixBuf_t* frameBuf;
	Bmp_t frame;         // the background picture
	int32_t textVisible; // the text layer takes part in the composite
	PixBuf_t* textBuf;
	Bmp_t text;        // what the text output draws into
	int32_t textLevel; // transparency of the text layer (0 opaque .. 0x100)
	Rect_t textArea;   // where characters may go (text-layer coordinates)
	WinItem_t item[WIN_ITEMS];
	int32_t subCount;         // slots in `sub`
	DispObj_t** sub;          // subCount projected sprites (or NULL)
	ScreenBmp_t subSurface;   // alias of `comp` + its rectangle: what subComp renders into
	int32_t pad2A4[7];        // zeroed with subSurface
	Compositor_t* subComp;    // the private compositor of the sub-sprites (or NULL)
	int32_t subOffX, subOffY; // -(screenW / 2), -(screenH / 2): the world offset of a sub-sprite
	int32_t subDist;          // screenW / 2: the projection distance
	int32_t font;             // handle from the font cache (0: none)
	int32_t proportional;     // proportional-width glyph placement flag
	int32_t fontSize;         // the font's pixel size
	int32_t glyphW;           // fontSize * widthPct / 100
	int32_t spacing;          // 0..800, line gap in percent of fontSize
	int32_t reserveRuby;      // keep one fontSize free on the right
	int32_t curX, curY;       // text cursor, text-layer coordinates
	int32_t drawStyle;        // 0 horizontal, 1 vertical
	int32_t swingStyle;       // 0 left, 1 centred, 2 right-aligned
	int32_t hasTable;         // `table` was supplied
	int32_t table[16];        // 16 values supplied by "90 A7": the per-item colours of a text menu
	int32_t punchItems;       // cut the item shapes out of the text
	int32_t layerOrder[3];    // 1.494: the passes of the composite ("90 82"): 0 frame, 1 text with its items, 2 sub-sprites
} Window_t;

extern const DispObjVtbl_t Window_Vtbl;
extern int gWindowsShown; // visibility switch for all windows ("90 0C")
extern int gWindowsLevel; // transparency added to all windows ("90 0C")

void Window_SetGlobalShown(int f);
void Window_SetGlobalLevel(int level);

void Window_Ctor(Window_t* w, int slotId);
void Window_Dtor(Window_t* w);

/* allocate the three layers; small numbers are counts of 32-pixel
 * cells.  Returns `ready` (the setSize result, 1), 0 when the size is out
 * of range (1..0x400 x 1..0x300 up to 1.494, 1..0x780 x 1..0x8000 from
 * 1.529 on). */
int Window_CreateSurfaces(Window_t* w, int cw, int ch);
int Window_CopyComposite(Window_t* w, Bmp_t* dst); // "90 83"; returns ready

// frame layer; results 0 ok, 1 not ready, 2 bitmap missing
int Window_SetFrame(Window_t* w, int bmp);                                               // "90 86"; bmp -1 clears
void Window_Set(Window_t* w, int x, int y, int effect, int level, int unused, int prio); // "90 85"
void Window_SetPunch(Window_t* w, int f);                                                // "90 87"
int Window_SetLayerOrder(Window_t* w, int order);                                        // "90 82" (1.494 on): 0.. 5, 1 when bad
void Window_ShowFrame(Window_t* w, int f);                                               // "92 88"
uint32_t Window_FillFrame(Window_t* w, uint32_t colour);                                 // "92 8A": colour, or 1 when not ready
// "92 89": 0 ok, 1 not ready, 2 no bitmap, 4..7 blit errors; *outScreen the touched screen rectangle
int Window_DrawToFrame(Window_t* w, Rect_t* outScreen, int x, int y, int bmp, int effect, int level);

// text layer
void Window_ShowText(Window_t* w, int f); // "92 8C" (the caller recomposites)
void Window_SetTextLevel(Window_t* w, int level);
// "92 8D": 0 ok, 2 no bitmap, 4..7 blit errors; *outScreen the touched screen rectangle
int Window_DrawToText(Window_t* w, Rect_t* outScreen, int x, int y, int bmp, int effect, int level);
// the same with the bitmap given (the engine's own drawing into the text layer)
int Window_DrawBmpToText(Window_t* w, Rect_t* outScreen, int x, int y, const Bmp_t* src, int effect, int level);
int Window_DrawText(Window_t* w, const char* str, int parseTags, int hang, uint32_t colour,
	uint32_t rubyColour, const int32_t style[5]);                  // "91 91" / "91 93": 1 drawn, 0 not
int Window_SetTextAreaRect(Window_t* w, const Rect_t* r);          // 1 ok, 0 out of the layer
int Window_SetTextArea(Window_t* w, int x, int y, int cw, int ch); // "90 88": the same from a position and size
void Window_ClearText(Window_t* w);
int Window_Scroll(Window_t* w, int n);                // move the text up n pixels; 1 if scrolled
void Window_GetTextArea(Window_t* w, Rect_t* out);    // "90 89"
void Window_UsableArea(Window_t* w, Rect_t* out);     // the text area plus the ruby reservation
void Window_TextScreenRect(Window_t* w, Rect_t* out); // the text layer in screen coordinates

// items
void Window_ItemEnable(Window_t* w, int i, int f);
void Window_ItemSet(Window_t* w, int i, int x, int y, int level);
int Window_ItemSetBitmap(Window_t* w, int i, const Bmp_t* src); // 0 ok, 4 bad mode
void Window_ItemsDisableAll(Window_t* w);
int Window_ItemRect(Window_t* w, Rect_t* out, int i);          // screen rectangle; 1 if shown
int Window_ItemRefresh(Window_t* w, int i, int forcedEnabled); // recomposite its footprint; i < 8

// sub-sprites (the sprite panel, "90 B7" / "91 BA")
void Window_SubAlloc(Window_t* w, int n); // n slots (0 frees everything)
// a projected sprite of `bmp` in slot i at (x, y, z), pivot (ox, oy); 0 ok, 9 bad index, 2 bad bitmap
int Window_SubCreate(Window_t* w, int i, int bmp, int x, int y, int z, int ox, int oy, int32_t angle, int prio);
int Window_SubSetBitmap(Window_t* w, int i, int bmp);         // 0 ok, 9 bad index, 2 bad bitmap
int Window_SubSetAngle(Window_t* w, int i, int32_t angle);    // 0 ok, 9 bad index
int Window_SubRefresh(Window_t* w, Rect_t* outScreen, int i); // 1 ok, 0 bad index
int Window_SubErase(Window_t* w, Rect_t* outScreen, int i);   // 1 ok, 0 bad index

// fonts and layout
int Window_RubyReserve(Window_t* w);                                                 // pixels kept free on the right (fontSize or 0)
int Window_SetFont(Window_t* w, const char* face, int size, int widthPct, int bold); // "91 88": 0 ok or the font cache's error
int Window_GetFont(Window_t* w);
int Window_GetFontSize(Window_t* w);
int Window_SetSpacing(Window_t* w, int pct); // "91 89": 1 ok, 0 not 0..800
int Window_GetSpacing(Window_t* w);
int Window_SpacingPixels(Window_t* w); // the line gap in pixels
int Window_LineAdvance(Window_t* w);   // fontSize + the gap
int Window_LineCount(Window_t* w);     // lines that fit the text area
void Window_SetProportional(Window_t* w, int v);
int Window_GetProportional(Window_t* w);
void Window_SetRubyReserve(Window_t* w, int f);
int Window_GetRubyReserve(Window_t* w);
int Window_SetDrawStyle(Window_t* w, int style); // "91 8A": 1 ok, 0 not 0..1
int Window_GetDrawStyle(Window_t* w);
int Window_SetSwingStyle(Window_t* w, int style); // "91 8B": 1 ok, 0 not 0..2
int Window_GetSwingStyle(Window_t* w);

// cursor
void Window_ResetCursor(Window_t* w);                          // to the start of the text area
int Window_NewLine(Window_t* w);                               // 1 if there was room
int Window_CursorFits(Window_t* w, int n);                     // does an n-pixel advance stay inside the area
void Window_CursorAdvance(Window_t* w, int n);                 // along the writing direction
void Window_SetCursor(Window_t* w, int x, int y);              // "91 8C"
void Window_GetCursor(Window_t* w, int32_t out[2]);            // "91 8D"
int Window_CursorAtLineStart(Window_t* w);                     // "91 8E"
void Window_SetTable(Window_t* w, const int32_t* tableOrNull); // "90 A7"
int Window_GetTable(Window_t* w, int32_t out[16]);             // hasTable

// compositing
int Window_RecompositeAll(Window_t* w);                                // returns ready
int Window_Recomposite(Window_t* w, const Rect_t* r);                  // returns ready
int Window_RecompositeAt(Window_t* w, int x, int y, const Bmp_t* src); // the footprint of `src` at (x, y)

#endif // BGI_GFX_WINDOW_H_
