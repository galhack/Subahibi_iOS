/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * host.c - the Apple back end's host side (inc/bgi/apple_host.h): the
 *          engine thread, the game's directories and the redirection of
 *          the patch files and the saves, and the calls the application
 *          makes from its own thread
 *
 * The engine thread makes the game directory its current directory (the
 * POSIX layer's "C:\"), runs Engine_Main and reports the end.  Paths below
 * "C:\" pass through Redirect (gPosixPathRedirect, src/os/posix/paths.c):
 *
 *   * UserData\... and BGI.gdb - the saves and the global database - go to
 *     the save directory, so that the game data can be replaced (or live in
 *     a purgeable cache, as on tvOS) without losing progress; the game's own
 *     BGI.gdb is copied there once, which keeps the settings and read
 *     marks of a copy played elsewhere;
 *   * a file of the overlay directory (the loose files of a translation
 *     patch) shadows the game's file of the same name, case aside, the way
 *     loose files next to the original's executable shadow its archives.
 */
#include "apple_internal.h"
#include "bgi/engine.h"

#include <dirent.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

char gAppleGameDir[1024];
char gAppleOverlayDir[1024];
char gAppleSaveDir[1024];
int gAppleChinese;

static char gFontFile[1024];
static char gEngineArgs[512];
static pthread_t gThread;
static volatile int gRunning;
static void (*gEndHandler)(int code);
static AppleHostMessageBox_t gMessageBox;

// the overlay's file names, read once at the start (a patch has a few hundred)
static char** gOverlayNames;
static int gOverlayCount;

// ---- configuration --------------------------------------------------------------------------

// `src` into `dst` (n bytes) with a trailing '/'; "" for NULL or ""
static void SetDir(char* dst, size_t n, const char* src)
{
	size_t len;
	dst[0] = 0;
	if(!src || !src[0])
		return;
	snprintf(dst, n, "%s", src);
	len = strlen(dst);
	if(len && dst[len - 1] != '/' && len + 1 < n)
	{
		dst[len] = '/';
		dst[len + 1] = 0;
	}
}

static void OverlayFree(void)
{
	int i;
	for(i = 0; i < gOverlayCount; i++)
		free(gOverlayNames[i]);
	free(gOverlayNames);
	gOverlayNames = NULL;
	gOverlayCount = 0;
}

// list the overlay directory's plain files
static void OverlayLoad(void)
{
	DIR* d;
	struct dirent* e;
	int cap = 0;
	OverlayFree();
	if(!gAppleOverlayDir[0] || !(d = opendir(gAppleOverlayDir)))
		return;
	while((e = readdir(d)) != NULL)
	{
		if(e->d_name[0] == '.')
			continue;
		if(gOverlayCount == cap)
		{
			char** grown;
			cap = cap ? cap * 2 : 256;
			grown = (char**)realloc(gOverlayNames, (size_t)cap * sizeof *grown);
			if(!grown)
				break;
			gOverlayNames = grown;
		}
		gOverlayNames[gOverlayCount++] = strdup(e->d_name);
	}
	closedir(d);
}

// the overlay's spelling of `name` (case aside), NULL when it has none
static const char* OverlayFind(const char* name)
{
	int i;
	for(i = 0; i < gOverlayCount; i++)
		if(strcasecmp(gOverlayNames[i], name) == 0)
			return gOverlayNames[i];
	return NULL;
}

// ---- the redirection --------------------------------------------------------------------------

// 1 for the paths that belong to the player: UserData and everything below it, BGI.gdb
static int IsSavePath(const char* rel)
{
	size_t n = strcspn(rel, "/");
	if(n == 8 && strncasecmp(rel, "UserData", 8) == 0)
		return 1;
	return strcasecmp(rel, "BGI.gdb") == 0;
}

static int Redirect(const char* rel, char* out, size_t n)
{
	if(gAppleSaveDir[0] && IsSavePath(rel))
	{
		snprintf(out, n, "%s%s", gAppleSaveDir, rel);
		return 1;
	}
	if(gOverlayCount && !strchr(rel, '/'))
	{
		const char* hit = OverlayFind(rel);
		if(hit)
		{
			snprintf(out, n, "%s%s", gAppleOverlayDir, hit);
			return 1;
		}
	}
	return 0;
}

// copy a file (the game's BGI.gdb into the save directory); 1 when copied
static int CopyFile(const char* from, const char* to)
{
	FILE *in = fopen(from, "rb"), *out;
	char buf[0x10000];
	size_t got;
	int ok = 1;
	if(!in)
		return 0;
	out = fopen(to, "wb");
	if(!out)
	{
		fclose(in);
		return 0;
	}
	while((got = fread(buf, 1, sizeof buf, in)) > 0)
		if(fwrite(buf, 1, got, out) != got)
			ok = 0;
	fclose(in);
	if(fclose(out) != 0)
		ok = 0;
	return ok;
}

// the save directory with its UserData folder, seeded with the game's BGI.gdb on the first run
static void PrepareSaveDir(void)
{
	char path[1200], game[1200];
	struct stat st;
	if(!gAppleSaveDir[0])
		return;
	mkdir(gAppleSaveDir, 0755);
	snprintf(path, sizeof path, "%sUserData", gAppleSaveDir);
	mkdir(path, 0755);
	snprintf(path, sizeof path, "%sBGI.gdb", gAppleSaveDir);
	snprintf(game, sizeof game, "%sBGI.gdb", gAppleGameDir);
	if(stat(path, &st) != 0 && stat(game, &st) == 0)
		CopyFile(game, path);
}

// ---- the engine thread --------------------------------------------------------------------------

static void* EngineThread(void* arg)
{
	char cmd[0x400];
	int r = -1;
	BGI_UNUSED(arg);
	if(chdir(gAppleGameDir) == 0)
	{
		OS_SetArgv0("bgi");
		if(OS_Init())
		{
			gPosixPathRedirect = Redirect;
			if(gFontFile[0])
				AppleFont_Register(gFontFile);
			snprintf(cmd, sizeof cmd, "%s", gEngineArgs);
			r = Engine_Main(cmd);
			OS_Shutdown();
		}
	}
	gRunning = 0;
	if(gEndHandler)
		gEndHandler(r);
	return NULL;
}

int AppleHost_Start(const AppleHostConfig_t* cfg)
{
	pthread_attr_t attr;
	struct stat st;
	if(gRunning || !cfg || !cfg->gameDir || stat(cfg->gameDir, &st) != 0 || !S_ISDIR(st.st_mode))
		return 0;
	SetDir(gAppleGameDir, sizeof gAppleGameDir, cfg->gameDir);
	SetDir(gAppleOverlayDir, sizeof gAppleOverlayDir, cfg->overlayDir);
	SetDir(gAppleSaveDir, sizeof gAppleSaveDir, cfg->saveDir);
	snprintf(gFontFile, sizeof gFontFile, "%s", cfg->fontFile ? cfg->fontFile : "");
	snprintf(gEngineArgs, sizeof gEngineArgs, "%s", cfg->engineArgs ? cfg->engineArgs : "");
	gAppleChinese = cfg->chinese != 0;
	OverlayLoad();
	PrepareSaveDir();
	AppleWindow_Reset();
	gRunning = 1;
	pthread_attr_init(&attr);
	// the engine keeps large buffers on its stack (paths, text lines): 16 MB, as the main thread of a desktop process
	pthread_attr_setstacksize(&attr, 16u << 20);
	if(pthread_create(&gThread, &attr, EngineThread, NULL) != 0)
	{
		pthread_attr_destroy(&attr);
		gRunning = 0;
		return 0;
	}
	pthread_attr_destroy(&attr);
	pthread_detach(gThread);
	return 1;
}

int AppleHost_Running(void)
{
	return gRunning;
}

void AppleHost_RequestClose(void)
{
	AppleWindow_PushEvent(APPLE_EV_CLOSE, 0, 0, 0, 0);
}

void AppleHost_SetEndHandler(void (*handler)(int code))
{
	gEndHandler = handler;
}

// ---- input --------------------------------------------------------------------------------------

void AppleHost_PointerMove(int x, int y)
{
	AppleWindow_PushEvent(APPLE_EV_MOVE, x, y, 0, 0);
}

void AppleHost_PointerButton(int button, int down, int x, int y)
{
	AppleWindow_PushEvent(APPLE_EV_BUTTON, button, down != 0, x, y);
}

void AppleHost_Wheel(int delta)
{
	AppleWindow_PushEvent(APPLE_EV_WHEEL, delta, 0, 0, 0);
}

void AppleHost_Key(int vk, int down)
{
	AppleWindow_PushEvent(APPLE_EV_KEY, vk & 0xff, down != 0, 0, 0);
}

void AppleHost_SetTouchInput(int touch)
{
	extern int gPointerSuppressed; // src/sys/input.c ("B0 06")
	gPointerSuppressed = touch != 0;
}

void AppleHost_SetActive(int active)
{
	AppleWindow_PushEvent(APPLE_EV_ACTIVATE, active != 0, 0, 0, 0);
}

// ---- picture ------------------------------------------------------------------------------------

int AppleHost_LockFrame(uint32_t lastSerial, AppleHostFrame_t* out)
{
	pthread_mutex_lock(&gAppleFrameLock);
	if(!gAppleFrame || (lastSerial && gAppleFrameSerial == lastSerial))
	{
		pthread_mutex_unlock(&gAppleFrameLock);
		return 0;
	}
	out->pixels = gAppleFrame;
	out->width = gAppleFrameW;
	out->height = gAppleFrameH;
	out->pitch = gAppleFrameW * 4;
	out->serial = gAppleFrameSerial;
	return 1;
}

void AppleHost_UnlockFrame(void)
{
	pthread_mutex_unlock(&gAppleFrameLock);
}

int AppleHost_LockMovieFrame(uint32_t lastSerial, AppleHostMovieFrame_t* out, int* playing)
{
	return AppleMovie_Lock(lastSerial, out, playing);
}

void AppleHost_UnlockMovieFrame(void)
{
	AppleMovie_Unlock();
}

void AppleHost_ClientSize(int* w, int* h)
{
	pthread_mutex_lock(&gAppleFrameLock);
	*w = gAppleFrame ? gAppleFrameW : 0;
	*h = gAppleFrame ? gAppleFrameH : 0;
	pthread_mutex_unlock(&gAppleFrameLock);
}

// ---- dialogs ------------------------------------------------------------------------------------

void AppleHost_SetMessageBoxHandler(AppleHostMessageBox_t handler)
{
	gMessageBox = handler;
}

// the application's answer to a message box (window.c); -1 without a handler
int AppleHost_AskMessageBox(const char* text, const char* caption, uint32_t flags)
{
	return gMessageBox ? gMessageBox(text, caption, flags) : -1;
}
