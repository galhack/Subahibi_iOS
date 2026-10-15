/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * window.c - the Apple back end's main window: an image of the client area
 *            that the application draws (with Metal), the input the
 *            application queues, the cursor and key state, the dialogs,
 *            and the stubs of what a phone, a tablet, a TV or a headset
 *            has no counterpart for
 *
 * There is no window system between the engine and the screen: the
 * engine's presents (OS_BlitToWindow, OS_StretchToWindow) write into
 * gAppleFrame, an image of the client size (the display mode's size in full
 * screen), and the application uploads it whenever gAppleFrameSerial
 * changed (AppleHost_LockFrame, host.c) and scales it to the screen.  The
 * client area therefore always starts at (0, 0) of the "screen": client
 * and screen coordinates are the same.
 *
 * Input reaches the queue from the application's thread (host.c) and the
 * engine thread takes one message per OS_PumpMessages, as the original's
 * PeekMessage loop does.  Child windows ("B0 1x") and the text entry
 * ("B0 2x") do not exist here: their functions fail or do nothing, which
 * the engine reports the way it reports a window it could not create;
 * SubarashikihibiHD uses neither.  Message boxes go to the application
 * (AppleHost_SetMessageBoxHandler).
 */
#include "apple_internal.h"

#include <stdlib.h>

pthread_mutex_t gAppleFrameLock = PTHREAD_MUTEX_INITIALIZER;
uint32_t* gAppleFrame;
int gAppleFrameW, gAppleFrameH;
uint32_t gAppleFrameSerial;

static OsEventHandlers_t gH;                      // the engine's callbacks (OS_SetEventHandlers)
static int gAlive, gShown, gActive = 1;           // the window exists / is visible / has the focus
static int gFullscreen;                           // the window covers a display mode
static int gClientW = 800, gClientH = 600;        // the windowed client size
static int gModeW, gModeH;                        // the display mode of full screen (0 = none yet)
static int gCursorShape = OS_CURSOR_ARROW;        // OS_CursorSetShape
static int gCursorCount;                          // the ShowCursor counter (shown at >= 0)
static volatile int gQuitPosted;                  // OS_PostQuit, delivered by the next pump
static volatile int gCloseRequested;              // OS_WindowClose
static uint8_t gKeys[256];                        // 1 per virtual key held down
static int gMouseX, gMouseY;                      // the pointer in client pixels
static int gLastClickBtn = -1;                    // double-click detection: the last press's button,
static uint32_t gLastClickTime;                   // its time,
static int gLastClickX, gLastClickY;              // and its place
static int gDc;                                   // the one device context: its address is the handle

// the virtual keys of the mouse buttons: VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2
static const int kButtonVk[5] = {0x01, 0x02, 0x04, 0x05, 0x06};

// ---- the event queue ------------------------------------------------------------------------

typedef struct AppleEvent
{
	int type, a, b, c, d;
} AppleEvent_t;

#define QUEUE_SIZE 1024
static AppleEvent_t gQueue[QUEUE_SIZE];
static int gQueueHead, gQueueTail; // read at head, written at tail
static pthread_mutex_t gQueueLock = PTHREAD_MUTEX_INITIALIZER;

/* Queue an event from any thread.  A full queue drops the oldest pointer
 * move first (it carries no state that a later one does not), else the
 * new event. */
void AppleWindow_PushEvent(int type, int a, int b, int c, int d)
{
	int next;
	pthread_mutex_lock(&gQueueLock);
	if(type == APPLE_EV_MOVE && gQueueTail != gQueueHead)
	{ // coalesce consecutive moves: only the newest position matters
		int last = (gQueueTail + QUEUE_SIZE - 1) % QUEUE_SIZE;
		if(gQueue[last].type == APPLE_EV_MOVE)
		{
			gQueue[last].a = a;
			gQueue[last].b = b;
			pthread_mutex_unlock(&gQueueLock);
			return;
		}
	}
	next = (gQueueTail + 1) % QUEUE_SIZE;
	if(next != gQueueHead)
	{
		gQueue[gQueueTail].type = type;
		gQueue[gQueueTail].a = a;
		gQueue[gQueueTail].b = b;
		gQueue[gQueueTail].c = c;
		gQueue[gQueueTail].d = d;
		gQueueTail = next;
	}
	pthread_mutex_unlock(&gQueueLock);
}

static int PopEvent(AppleEvent_t* out)
{
	int ok = 0;
	pthread_mutex_lock(&gQueueLock);
	if(gQueueHead != gQueueTail)
	{
		*out = gQueue[gQueueHead];
		gQueueHead = (gQueueHead + 1) % QUEUE_SIZE;
		ok = 1;
	}
	pthread_mutex_unlock(&gQueueLock);
	return ok;
}

void AppleWindow_Reset(void)
{
	pthread_mutex_lock(&gQueueLock);
	gQueueHead = gQueueTail = 0;
	pthread_mutex_unlock(&gQueueLock);
	gQuitPosted = gCloseRequested = 0;
	memset(gKeys, 0, sizeof gKeys);
	gActive = 1;
}

// ---- the window ----------------------------------------------------------------------------------

// the window image takes the size the engine presents at: the mode in full screen, else the client size
static void ApplySize(void)
{
	int w = gFullscreen && gModeW > 0 ? gModeW : gClientW;
	int h = gFullscreen && gModeH > 0 ? gModeH : gClientH;
	pthread_mutex_lock(&gAppleFrameLock);
	if(!gAppleFrame || w != gAppleFrameW || h != gAppleFrameH)
	{
		free(gAppleFrame);
		gAppleFrame = (uint32_t*)calloc((size_t)w * h, 4);
		gAppleFrameW = gAppleFrame ? w : 0;
		gAppleFrameH = gAppleFrame ? h : 0;
		gAppleFrameSerial++;
	}
	pthread_mutex_unlock(&gAppleFrameLock);
}

int OS_WindowCreate(const OsWindowDesc_t* d)
{
	if(gAlive)
		return 0;
	gClientW = d->width;
	gClientH = d->height;
	gAlive = 1;
	gShown = 0;
	gFullscreen = 0;
	memset(gKeys, 0, sizeof gKeys);
	ApplySize();
	if(gH.created)
		gH.created();
	return 1;
}

void OS_WindowDestroy(void)
{
	if(!gAlive)
		return;
	gAlive = 0;
	gShown = 0;
	if(gH.destroyed)
		gH.destroyed();
}

int OS_WindowAlive(void)
{
	return gAlive;
}

// a show queues the activation and a paint, as the desktop sends them
void OS_WindowShow(int show)
{
	if(!gAlive)
		return;
	gShown = show != 0;
	if(show)
	{
		AppleWindow_PushEvent(APPLE_EV_ACTIVATE, 1, 0, 0, 0);
		AppleWindow_PushEvent(APPLE_EV_PAINT, 0, 0, 0, 0);
	}
}

void OS_WindowSetTitle(const char* t)
{
	BGI_UNUSED(t); // the application shows its own name
}

void OS_WindowMove(int x, int y)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
}

void OS_WindowGetRect(int* x, int* y, int* w, int* h)
{
	*x = 0;
	*y = 0;
	*w = gFullscreen ? gModeW : gClientW;
	*h = gFullscreen ? gModeH : gClientH;
}

int OS_WindowMinimized(void)
{
	return 0;
}

int OS_WindowActive(void)
{
	return gActive;
}

void OS_WindowMinimize(void)
{
}

void OS_WindowClose(void)
{
	gCloseRequested = 1;
}

// SIGTERM / SIGINT of the POSIX process code: the same as the close box
void OsPosix_RequestClose(void)
{
	gCloseRequested = 1;
}

/* The "desktop": a 1920 x 1080 screen, larger than the game's 1280 x 960 in
 * both directions, so the boot program sizes its window to the full
 * picture (it shrinks it to the desktop otherwise); the application
 * scales that picture to the real screen. */
void OS_ScreenSize(int* w, int* h)
{
	*w = 1920;
	*h = 1080;
}

void OS_FrameMetrics(int* fw, int* fh)
{
	*fw = *fh = 0;
}

int OS_FrameTop(void)
{
	return 0;
}

void OS_ImeStatus(int open)
{
	BGI_UNUSED(open);
}

// no mode is switched: the size is kept for the window image of full screen
int OS_DisplaySetMode(int fullscreen, int w, int h, int bpp)
{
	BGI_UNUSED(fullscreen);
	BGI_UNUSED(bpp);
	if(w <= 0 || h <= 0)
		return 0;
	gModeW = w;
	gModeH = h;
	return 1;
}

void OS_DisplayRestore(void)
{
}

void OS_WindowSetFullscreen(int modeW, int modeH)
{
	gModeW = modeW;
	gModeH = modeH;
	gFullscreen = 1;
	ApplySize();
}

void OS_WindowSetWindowed(int x, int y, int clientW, int clientH)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	gFullscreen = 0;
	gClientW = clientW;
	gClientH = clientH;
	ApplySize();
}

// ---- presenting ----------------------------------------------------------------------------------

// one source pixel as 0x00RRGGBB (16-bit surfaces are RGB555, each 5-bit channel widened to 8)
static uint32_t SurfacePixel(const OsSurface_t* s, int x, int y)
{
	const uint8_t* row = (const uint8_t*)s->pixels + (ptrdiff_t)y * s->pitch;
	if(s->bpp == 16)
	{
		uint32_t v = ((const uint16_t*)row)[x];
		uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
		return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
	}
	return ((const uint32_t*)row)[x] & 0xffffff;
}

/* The source rectangle (srcX, srcY, srcW x srcH of the surface) to the
 * destination rectangle of the window image, clipped to `clip` ({l, t, r,
 * b}, right / bottom exclusive; NULL for none) and to the image: a row copy
 * when the sizes match (the usual case: the back buffer is the client
 * size), nearest neighbour otherwise (the GPU filters the final scaling).
 * Source pixels outside the surface read as black. */
static void BlitSurface(int dstX, int dstY, int dstW, int dstH, const OsSurface_t* s, int srcX, int srcY, int srcW,
	int srcH, const int* clip)
{
	int cl = 0, ct = 0, cr, cb, x, y;
	if(!gAlive || dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0 || !s->pixels)
		return;
	pthread_mutex_lock(&gAppleFrameLock);
	if(!gAppleFrame)
	{
		pthread_mutex_unlock(&gAppleFrameLock);
		return;
	}
	cr = gAppleFrameW;
	cb = gAppleFrameH;
	if(clip)
	{
		cl = BGI_MAX(cl, clip[0]);
		ct = BGI_MAX(ct, clip[1]);
		cr = BGI_MIN(cr, clip[2]);
		cb = BGI_MIN(cb, clip[3]);
	}
	cl = BGI_MAX(cl, dstX);
	ct = BGI_MAX(ct, dstY);
	cr = BGI_MIN(cr, dstX + dstW);
	cb = BGI_MIN(cb, dstY + dstH);
	if(cr > cl && cb > ct)
	{
		if(srcW == dstW && srcH == dstH)
		{
			for(y = ct; y < cb; y++)
			{
				int sy = srcY + (y - dstY);
				int sx0 = srcX + (cl - dstX);
				uint32_t* o = gAppleFrame + (size_t)y * gAppleFrameW + cl;
				if(sy < 0 || sy >= s->height)
				{
					memset(o, 0, (size_t)(cr - cl) * 4);
					continue;
				}
				if(s->bpp == 32 && sx0 >= 0 && sx0 + (cr - cl) <= s->width)
				{ // the common case: straight 32-bit rows
					const uint32_t* row = (const uint32_t*)((const uint8_t*)s->pixels + (ptrdiff_t)sy * s->pitch) + sx0;
					memcpy(o, row, (size_t)(cr - cl) * 4);
					continue;
				}
				for(x = 0; x < cr - cl; x++)
				{
					int sx = sx0 + x;
					o[x] = sx >= 0 && sx < s->width ? SurfacePixel(s, sx, sy) : 0;
				}
			}
		}
		else
		{
			for(y = ct; y < cb; y++)
			{
				int sy = srcY + (int)((int64_t)(y - dstY) * srcH / dstH);
				uint32_t* o = gAppleFrame + (size_t)y * gAppleFrameW;
				for(x = cl; x < cr; x++)
				{
					int sx = srcX + (int)((int64_t)(x - dstX) * srcW / dstW);
					o[x] = sx >= 0 && sx < s->width && sy >= 0 && sy < s->height ? SurfacePixel(s, sx, sy) : 0;
				}
			}
		}
		gAppleFrameSerial++;
	}
	pthread_mutex_unlock(&gAppleFrameLock);
}

OsDc_t* OS_WindowGetDc(void)
{
	return gAlive ? &gDc : NULL;
}

void OS_WindowReleaseDc(OsDc_t* dc)
{
	BGI_UNUSED(dc);
}

void OS_BlitToWindow(OsDc_t* dc, int dstX, int dstY, int w, int h, const OsSurface_t* src, int srcX, int srcY)
{
	if(dc && dc != (OsDc_t*)&gDc)
		return; // a child window's context: none exist
	BlitSurface(dstX, dstY, w, h, src, srcX, srcY, w, h, NULL);
}

void OS_StretchToWindow(OsDc_t* dc, int dstX, int dstY, int dstW, int dstH, const OsSurface_t* src, const int* clip)
{
	if(dc && dc != (OsDc_t*)&gDc)
		return;
	BlitSurface(dstX, dstY, dstW, dstH, src, 0, 0, src->width, src->height, clip);
}

void OS_WindowFillBlack(void)
{
	pthread_mutex_lock(&gAppleFrameLock);
	if(gAppleFrame)
	{
		memset(gAppleFrame, 0, (size_t)gAppleFrameW * gAppleFrameH * 4);
		gAppleFrameSerial++;
	}
	pthread_mutex_unlock(&gAppleFrameLock);
}

// ---- cursor and keys ------------------------------------------------------------------------------

void OS_CursorSetShape(int shape)
{
	gCursorShape = shape; // the application draws no cursor of its own; kept for symmetry
	BGI_UNUSED(gCursorShape);
}

void OS_CursorShow(int show)
{
	gCursorCount += show ? 1 : -1;
}

void OS_CursorGetPos(int* x, int* y)
{
	*x = gMouseX;
	*y = gMouseY;
}

void OS_CursorSetPos(int x, int y)
{
	gMouseX = x; // the engine moves the "cursor" onto a button for the keyboard; nothing to show
	gMouseY = y;
}

void OS_ScreenToClient(int* x, int* y)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y); // the client area is the screen
}

void OS_ClientToScreen(int* x, int* y)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
}

int OS_MouseButtonsSwapped(void)
{
	return 0;
}

int OS_KeyState(int vk)
{
	return gKeys[vk & 0xff] ? 1 : 0;
}

void OS_KeyboardState(uint8_t out[256])
{
	int i;
	for(i = 0; i < 256; i++)
		out[i] = gKeys[i] ? 0x80 : 0;
}

/* a key or a button changed: VK_SHIFT / VK_CONTROL / VK_MENU follow their
 * left and right variants as on Windows */
static void SetKey(int vk, int down)
{
	gKeys[vk & 0xff] = (uint8_t)(down != 0);
	if(vk == 0xa0 || vk == 0xa1)
		gKeys[0x10] = gKeys[0xa0] | gKeys[0xa1];
	if(vk == 0xa2 || vk == 0xa3)
		gKeys[0x11] = gKeys[0xa2] | gKeys[0xa3];
	if(vk == 0xa4 || vk == 0xa5)
		gKeys[0x12] = gKeys[0xa4] | gKeys[0xa5];
}

// ---- child windows, text entry: none --------------------------------------------------------------

OsChild_t* OS_ChildCreate(int index, const char* title, int x, int y, int outerW, int outerH)
{
	BGI_UNUSED(index);
	BGI_UNUSED(title);
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(outerW);
	BGI_UNUSED(outerH);
	return NULL;
}

void OS_ChildClose(OsChild_t* c)
{
	BGI_UNUSED(c);
}

void OS_ChildShow(OsChild_t* c, int show)
{
	BGI_UNUSED(c);
	BGI_UNUSED(show);
}

void OS_ChildSetTitle(OsChild_t* c, const char* title)
{
	BGI_UNUSED(c);
	BGI_UNUSED(title);
}

void OS_ChildMove(OsChild_t* c, int x, int y, int outerW, int outerH)
{
	BGI_UNUSED(c);
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(outerW);
	BGI_UNUSED(outerH);
}

void OS_ChildGetPos(OsChild_t* c, int* x, int* y)
{
	BGI_UNUSED(c);
	*x = *y = 0;
}

OsDc_t* OS_ChildGetDc(OsChild_t* c)
{
	BGI_UNUSED(c);
	return NULL;
}

void OS_ChildReleaseDc(OsChild_t* c, OsDc_t* dc)
{
	BGI_UNUSED(c);
	BGI_UNUSED(dc);
}

void OS_EditShow(int show)
{
	BGI_UNUSED(show);
}

void OS_EditMove(int x, int y, int w, int h)
{
	BGI_UNUSED(x);
	BGI_UNUSED(y);
	BGI_UNUSED(w);
	BGI_UNUSED(h);
}

void OS_EditSetFont(const char* face, int size)
{
	BGI_UNUSED(face);
	BGI_UNUSED(size);
}

void OS_EditLimitText(int maxChars)
{
	BGI_UNUSED(maxChars);
}

void OS_EditSetText(const char* t)
{
	BGI_UNUSED(t);
}

void OS_EditSelectAll(void)
{
}

void OS_EditFocus(int toEdit)
{
	BGI_UNUSED(toEdit);
}

int OS_EditGetText(char* buf, size_t n)
{
	if(n)
		buf[0] = 0;
	return 0;
}

void OS_EditSetColour(uint32_t rgb)
{
	BGI_UNUSED(rgb);
}

void OS_EditRefresh(void)
{
}

void OS_EditDestroy(void)
{
}

void OS_EditRejectAscii(int reject)
{
	BGI_UNUSED(reject);
}

int OS_EditImeOpen(void)
{
	return 0;
}

// ---- dialogs ----------------------------------------------------------------------------------------

/* The default answer when no application handler exists: the default
 * button of the box (the second one with MB_DEFBUTTON2). */
static int DefaultButton(uint32_t flags)
{
	int second = (flags & OS_MB_DEFBUTTON2) != 0;
	switch(flags & 0xf)
	{
		case OS_MB_OKCANCEL: return second ? OS_IDCANCEL : OS_IDOK;
		case OS_MB_YESNO: return second ? OS_IDNO : OS_IDYES;
		case OS_MB_RETRYCANCEL: return second ? OS_IDCANCEL : OS_IDRETRY;
		default: return OS_IDOK;
	}
}

int OS_MessageBox(const char* text, const char* caption, uint32_t flags)
{
	char t[0x1000], c[0x400];
	int r;
	AppleText_ToUtf8(text, t, sizeof t);
	AppleText_ToUtf8(caption, c, sizeof c);
	fprintf(stderr, "bgi: message box \"%s\": %s\n", c, t);
	r = AppleHost_AskMessageBox(t, c, flags);
	return r < 0 ? DefaultButton(flags) : r;
}

int OS_DialogInput1(const char* title, const char* initial, int maxLen, char* out)
{
	BGI_UNUSED(title);
	BGI_UNUSED(initial);
	BGI_UNUSED(maxLen);
	BGI_UNUSED(out);
	return 0;
}

int OS_DialogInput2(const char* title, const char* label1, const char* initial1, int maxLen1, char* out1,
	const char* label2, const char* initial2, int maxLen2, char* out2)
{
	BGI_UNUSED(title);
	BGI_UNUSED(label1);
	BGI_UNUSED(initial1);
	BGI_UNUSED(maxLen1);
	BGI_UNUSED(out1);
	BGI_UNUSED(label2);
	BGI_UNUSED(initial2);
	BGI_UNUSED(maxLen2);
	BGI_UNUSED(out2);
	return 0;
}

int OS_DialogProfile(char* surname, char* givenName, char* nickname, char* pronoun, int32_t* month, int32_t* day)
{
	BGI_UNUSED(surname);
	BGI_UNUSED(givenName);
	BGI_UNUSED(nickname);
	BGI_UNUSED(pronoun);
	BGI_UNUSED(month);
	BGI_UNUSED(day);
	return 0;
}

int OS_FileDialog(int save, char* path, size_t n, const char* filter, const char* defExt, const char* initialDir, const char* title)
{
	BGI_UNUSED(save);
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(filter);
	BGI_UNUSED(defExt);
	BGI_UNUSED(initialDir);
	BGI_UNUSED(title);
	return 0;
}

int OS_BrowseFolder(char* path, size_t n, const char* title, const char* initial)
{
	BGI_UNUSED(path);
	BGI_UNUSED(n);
	BGI_UNUSED(title);
	BGI_UNUSED(initial);
	return 0;
}

// the launcher is the application's own screen: the engine never shows one here
int OS_LauncherDialog(OsLauncher_t* l)
{
	BGI_UNUSED(l);
	return -1;
}

int OS_ClipboardSetText(const char* text)
{
	BGI_UNUSED(text);
	return 0;
}

// no second window: the debugger, when asked for, draws over the game
int OS_DebugWindowOpen(int w, int h, const char* title)
{
	BGI_UNUSED(w);
	BGI_UNUSED(h);
	BGI_UNUSED(title);
	return 0;
}

void OS_DebugWindowClose(void)
{
}

int OS_DebugWindowAlive(void)
{
	return 0;
}

void OS_DebugWindowPresent(const OsSurface_t* s)
{
	BGI_UNUSED(s);
}

// ---- the message pump ---------------------------------------------------------------------------------

void OS_SetEventHandlers(const OsEventHandlers_t* h)
{
	gH = *h;
}

void OS_PostQuit(void)
{
	gQuitPosted = 1;
}

void OS_DragAccept(int accept)
{
	BGI_UNUSED(accept);
}

/* one queued event to the engine's callback, with the key and pointer
 * state kept up to date; a press of the same button within 400 ms and 4
 * pixels of the previous one raises `double_click` first, as Windows sends
 * WM_?BUTTONDBLCLK before the press; a focus loss releases every key */
static void Deliver(const AppleEvent_t* e)
{
	switch(e->type)
	{
		case APPLE_EV_KEY:
			SetKey(e->a, e->b);
			if(e->b && gH.key_down)
				gH.key_down(e->a, 0);
			else if(!e->b && gH.key_up)
				gH.key_up(e->a);
			break;
		case APPLE_EV_MOVE:
			gMouseX = e->a;
			gMouseY = e->b;
			if(gH.mouse_move)
				gH.mouse_move(e->a, e->b);
			break;
		case APPLE_EV_BUTTON:
			gMouseX = e->c;
			gMouseY = e->d;
			if(e->a >= 0 && e->a < 5)
				SetKey(kButtonVk[e->a], e->b);
			if(e->b)
			{
				uint32_t now = OS_TicksMs();
				if(e->a <= 1 && gLastClickBtn == e->a && now - gLastClickTime < 400 && BGI_ABS(e->c - gLastClickX) < 4 &&
					BGI_ABS(e->d - gLastClickY) < 4)
				{
					if(gH.double_click)
						gH.double_click(e->a);
					gLastClickBtn = -1;
				}
				else
				{
					gLastClickBtn = e->a;
					gLastClickTime = now;
					gLastClickX = e->c;
					gLastClickY = e->d;
				}
			}
			if(gH.mouse_button)
				gH.mouse_button(e->a, e->b, e->c, e->d);
			break;
		case APPLE_EV_WHEEL:
			if(gH.mouse_wheel)
				gH.mouse_wheel(e->a);
			break;
		case APPLE_EV_ACTIVATE:
			gActive = e->a;
			if(!e->a)
				memset(gKeys, 0, sizeof gKeys);
			if(gH.activate)
				gH.activate(e->a);
			if(e->a && gH.paint)
				gH.paint(); // coming back to the front: the picture is redrawn
			break;
		case APPLE_EV_CLOSE:
			if(gH.close_request)
				gH.close_request();
			break;
		case APPLE_EV_PAINT:
			if(gH.paint)
				gH.paint();
			break;
		case APPLE_EV_MOVIE:
			if(gH.movie_event)
				gH.movie_event(e->a);
			break;
		default: break;
	}
}

int OS_PumpMessages(void)
{
	AppleEvent_t e;
	if(gQuitPosted)
	{
		gQuitPosted = 0;
		if(gH.quit)
			gH.quit();
		return 1;
	}
	if(gCloseRequested)
	{
		gCloseRequested = 0;
		if(gH.close_request)
			gH.close_request();
		return 1;
	}
	if(!PopEvent(&e))
		return 0;
	Deliver(&e);
	return 1;
}
