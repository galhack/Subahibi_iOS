/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * apple_internal.h - the state shared by the files of the Apple back end
 *                    (src/os/apple/): the host thread and its configuration
 *                    (host.c), the window's event queue and picture
 *                    (window.c), the audio output and the movie player
 *                    (audio.c, movie.c), the fonts (font.c)
 *
 * The system services come from the POSIX back end (files, paths,
 * registry, time, threads, Shift-JIS through iconv: src/os/posix/), with
 * machine.c replaced; this directory holds what an iPhone, an iPad, an
 * Apple TV or a Vision Pro does differently.  The public side is
 * inc/bgi/apple_host.h.
 */
#ifndef BGI_APPLE_INTERNAL_H
#define BGI_APPLE_INTERNAL_H

#include "bgi/os.h"
#include "bgi/os_common.h"
#include "bgi/os_posix.h"
#include "bgi/apple_host.h"

#include <pthread.h>

// ---- host.c: configuration ------------------------------------------------------------------
extern char gAppleGameDir[1024];    // with a trailing '/'
extern char gAppleOverlayDir[1024]; // "" for none, else with a trailing '/'
extern char gAppleSaveDir[1024];    // "" for none, else with a trailing '/'
extern int gAppleChinese;           // the script text is GBK (font.c decodes it so)
// the application's answer to a message box (OS_IDOK, ...); -1 when it installed no handler
int AppleHost_AskMessageBox(const char* text, const char* caption, uint32_t flags);
int AppleOverlay_FindSupplement(const char* folder, const char* name, char* out, size_t n);

// ---- window.c: the event queue of the main window ----------------------------------------------
enum
{
	APPLE_EV_KEY = 1,  // a = virtual key, b = down
	APPLE_EV_MOVE,     // a = x, b = y
	APPLE_EV_BUTTON,   // a = button, b = down, c = x, d = y
	APPLE_EV_WHEEL,    // a = delta
	APPLE_EV_ACTIVATE, // a = active
	APPLE_EV_CLOSE,    // the application asked the engine to end
	APPLE_EV_PAINT,    // repaint the whole window
	APPLE_EV_MOVIE     // the movie player's notification: a = the movie completed
};
void AppleWindow_PushEvent(int type, int a, int b, int c, int d);
void AppleWindow_Reset(void); // before a new run of the engine

// ---- window.c: the picture ---------------------------------------------------------------------
// the window image (client size, 0x00RRGGBB) and its lock; host.c hands it to the application
extern pthread_mutex_t gAppleFrameLock;
extern uint32_t* gAppleFrame;
extern int gAppleFrameW, gAppleFrameH;
extern uint32_t gAppleFrameSerial;

// ---- movie.c ----------------------------------------------------------------------------------
extern pthread_mutex_t gAppleMovieLock;
/* mix the running movie's sound into `samples` (interleaved stereo int16 at
 * the output rate); called by audio.c on the audio thread after the
 * engine's mixer filled the buffer */
void AppleMovie_MixAudio(int16_t* samples, int frames, int rate);
int AppleMovie_Lock(uint32_t lastSerial, AppleHostMovieFrame_t* out, int* playing);
void AppleMovie_Unlock(void);

// ---- audio.c ------------------------------------------------------------------------------------
int AppleAudio_Rate(void); // the output rate of the running output, 0 when none

// ---- font.c -------------------------------------------------------------------------------------
int AppleFont_Register(const char* path); // a font file for the faces of OS_FontOpen; 1 ok
// engine text (Shift-JIS, or GBK under the Chinese patch) as UTF-8 for the application
void AppleText_ToUtf8(const char* text, char* out, size_t n);

#endif
