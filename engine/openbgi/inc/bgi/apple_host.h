/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * apple_host.h - the interface between the Apple back end (src/os/apple/)
 *                and the application that hosts it (iOS, iPadOS, tvOS,
 *                visionOS; the Swift shell of this port)
 *
 * The engine runs its own message loop, as the original's WinMain does, so
 * it lives on a thread of its own (AppleHost_Start).  The application's
 * main thread talks to it through this header only:
 *
 *   * input: AppleHost_Pointer* / AppleHost_Key* queue window messages that
 *     the engine's OS_PumpMessages delivers on its thread; coordinates are
 *     the game's client pixels (the application maps its view onto them);
 *   * picture: the engine's presents land in a "window image" of the client
 *     size; AppleHost_LockFrame hands it to the renderer when it changed,
 *     AppleHost_LockMovieFrame does the same for the movie playing over it;
 *   * dialogs: the engine's message boxes are forwarded to the application
 *     (AppleHost_SetMessageBoxHandler), which answers on its own thread
 *     while the engine thread waits;
 *   * life cycle: AppleHost_SetActive for the background / foreground
 *     transitions, AppleHost_RequestQuit to end the game, the end handler
 *     when the engine has returned.
 *
 * Every function is safe to call from any thread unless it says otherwise.
 */
#ifndef BGI_APPLE_HOST_H_
#define BGI_APPLE_HOST_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AppleHostConfig
{
	const char* gameDir;    // the game's files (the archives, ...), a POSIX path
	const char* overlayDir; // loose files that shadow the game directory (a translation patch), NULL for none
	const char* saveDir;    // where UserData\ and BGI.gdb are read and written, NULL for the game directory
	const char* fontFile;   // a font file to register before the start (the patch's font.ttf), NULL for none
	int chinese;            // the script text is GBK (the Chinese patch) rather than Shift-JIS
	const char* engineArgs; // extra command-line options for Engine_Main ("--engine=1.647"), NULL for none
} AppleHostConfig_t;

/* Start the engine on its own thread with the game of `cfg` (the strings are
 * copied).  1 when the thread runs, 0 when an engine runs already or the
 * game directory is unusable. */
int AppleHost_Start(const AppleHostConfig_t* cfg);
// 1 while the engine thread runs
int AppleHost_Running(void);
/* Ask the engine to end as if its window's close box was pressed (the game
 * may ask for confirmation with a message box); the end handler follows. */
void AppleHost_RequestClose(void);
// called on the engine thread when Engine_Main has returned (exit code); NULL for none
void AppleHost_SetEndHandler(void (*handler)(int code));

// ---- input (client pixels of the game: 0 .. width-1, 0 .. height-1) -----------------------
void AppleHost_PointerMove(int x, int y);
// button 0 left, 1 right, 2 middle
void AppleHost_PointerButton(int button, int down, int x, int y);
// WHEEL_DELTA (120) units, positive = away from the user (scroll back)
void AppleHost_Wheel(int delta);
// a Windows virtual-key code pressed or released (VK_RETURN 0x0D, VK_SPACE 0x20, VK_CONTROL 0x11, ...)
void AppleHost_Key(int vk, int down);
// 1 while the pointer is a touch (no hover), 0 for a mouse, trackpad or eye/hand pointer ("B0 06")
void AppleHost_SetTouchInput(int touch);
// the application came to the front (1) or went to the background (0)
void AppleHost_SetActive(int active);

// ---- picture --------------------------------------------------------------------------------
typedef struct AppleHostFrame
{
	const uint32_t* pixels; // 0x00RRGGBB, top-down rows
	int width, height;      // the client size
	int pitch;              // bytes per row
	uint32_t serial;        // increases with every change
} AppleHostFrame_t;
/* The window image: 1 with *out filled and the image locked when it is newer
 * than `lastSerial` (pass 0 to get it unconditionally), else 0 and nothing
 * is locked.  AppleHost_UnlockFrame releases it; keep the lock short (one
 * texture upload). */
int AppleHost_LockFrame(uint32_t lastSerial, AppleHostFrame_t* out);
void AppleHost_UnlockFrame(void);
/* The movie playing over the picture: its current frame (0x00RRGGBB) and the
 * rectangle it covers in client pixels.  1 with the frame locked when a
 * movie runs and the frame is newer than `lastSerial`; *playing tells
 * whether a movie runs at all (the renderer drops its overlay when not). */
typedef struct AppleHostMovieFrame
{
	const uint32_t* pixels;
	int width, height, pitch;
	int x, y, w, h; // the destination rectangle in client pixels
	uint32_t serial;
} AppleHostMovieFrame_t;
int AppleHost_LockMovieFrame(uint32_t lastSerial, AppleHostMovieFrame_t* out, int* playing);
void AppleHost_UnlockMovieFrame(void);
// the client size the engine asked for (1280 x 960 for this game); 0 x 0 before the window exists
void AppleHost_ClientSize(int* w, int* h);

// ---- dialogs --------------------------------------------------------------------------------
/* A message box of the engine: the text and caption in UTF-8 and the
 * MessageBox flags (OS_MB_* of os.h); the handler shows it and returns the
 * id of the button chosen (OS_IDOK, OS_IDYES, ...).  It is called on the
 * engine thread, which waits for the answer; NULL answers with the default
 * button. */
typedef int (*AppleHostMessageBox_t)(const char* text, const char* caption, uint32_t flags);
void AppleHost_SetMessageBoxHandler(AppleHostMessageBox_t handler);

#ifdef __cplusplus
}
#endif

#endif // BGI_APPLE_HOST_H_
