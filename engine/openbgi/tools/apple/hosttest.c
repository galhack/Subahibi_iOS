/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * hosttest.c - a command-line host for the Apple back end on macOS
 *              ("make apple" -> bin/openbgi-hosttest): it does what the
 *              iOS / tvOS / visionOS application does - start the engine
 *              on its thread through inc/bgi/apple_host.h, feed it input -
 *              with a scripted pointer instead of a touch screen, and
 *              writes the window image out instead of drawing it.
 *
 *   openbgi-hosttest GAME_DIR [options]
 *     --seconds=N        end after N seconds (default 30)
 *     --script="..."     wait S | move X Y | click X Y | rclick X Y |
 *                        key VK | wheel D | shot FILE  (as the wasm simulator)
 *     --shot=FILE        the last picture, written when the time is up
 *     --overlay=DIR      loose files shadowing the game (a translation patch)
 *     --font=FILE        a font file to register (the patch's font.ttf)
 *     --save=DIR         the save directory (UserData, BGI.gdb)
 *     --chinese          the script text is GBK
 *     --args="..."       options for Engine_Main
 *     --movie=ARC:NAME   no engine: play the movie NAME of the archive ARC
 *                        through the back end's player (OS_MovieOpenRange)
 *                        and write a picture of it every second as
 *                        movieN.png in the current directory
 *
 * The pictures are written as PNG through ImageIO, with the movie frame
 * drawn over the window image when a movie plays.
 */
#include "bgi/apple_host.h"
#include "bgi/os.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static volatile int gEnded = -1000;

static void OnEnd(int code)
{
	gEnded = code;
}

static int OnMessageBox(const char* text, const char* caption, uint32_t flags)
{
	fprintf(stderr, "hosttest: message box [%s] %s -> default\n", caption, text);
	(void)flags;
	return -1; // the back end answers with the default button
}

static double Now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// the window image (and the movie over it) as a PNG file
static void WriteShot(const char* path)
{
	AppleHostFrame_t f;
	AppleHostMovieFrame_t m;
	uint32_t* copy;
	int playing = 0, x, y;
	CGColorSpaceRef rgb;
	CGContextRef ctx;
	CGImageRef img;
	CFURLRef url;
	CGImageDestinationRef dst;
	if(!AppleHost_LockFrame(0, &f))
	{
		fprintf(stderr, "hosttest: no picture yet\n");
		return;
	}
	copy = (uint32_t*)malloc((size_t)f.width * f.height * 4);
	memcpy(copy, f.pixels, (size_t)f.width * f.height * 4);
	AppleHost_UnlockFrame();
	if(AppleHost_LockMovieFrame(0, &m, &playing))
	{ // nearest neighbour into the movie's rectangle
		for(y = 0; y < m.h; y++)
			for(x = 0; x < m.w; x++)
			{
				int dx = m.x + x, dy = m.y + y;
				if(dx < 0 || dy < 0 || dx >= f.width || dy >= f.height)
					continue;
				copy[(size_t)dy * f.width + dx] = m.pixels[(size_t)(y * m.height / m.h) * m.width + x * m.width / m.w];
			}
		AppleHost_UnlockMovieFrame();
	}
	for(x = 0; x < f.width * f.height; x++)
		copy[x] |= 0xff000000u;
	rgb = CGColorSpaceCreateDeviceRGB();
	ctx = CGBitmapContextCreate(copy, (size_t)f.width, (size_t)f.height, 8, (size_t)f.width * 4, rgb,
		kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
	img = CGBitmapContextCreateImage(ctx);
	url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
	dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
	CGImageDestinationAddImage(dst, img, NULL);
	if(!CGImageDestinationFinalize(dst))
		fprintf(stderr, "hosttest: cannot write %s\n", path);
	else
		fprintf(stderr, "hosttest: wrote %s (%dx%d)%s\n", path, f.width, f.height, playing ? " with the movie" : "");
	CFRelease(dst);
	CFRelease(url);
	CGImageRelease(img);
	CGContextRelease(ctx);
	CGColorSpaceRelease(rgb);
	free(copy);
}

// the value of --name=value, NULL when absent; --name alone gives ""
static const char* Opt(int argc, char** argv, const char* name)
{
	size_t n = strlen(name);
	int i;
	for(i = 2; i < argc; i++)
		if(strncmp(argv[i], name, n) == 0 && (argv[i][n] == '=' || argv[i][n] == 0))
			return argv[i][n] == '=' ? argv[i] + n + 1 : "";
	return NULL;
}

// the absolute offset and size of `name` in a BURIKO ARC20 archive; 1 when found
static int ArcFind(const char* arc, const char* name, uint32_t* off, uint32_t* size)
{
	FILE* f = fopen(arc, "rb");
	uint8_t hdr[16], e[0x80];
	uint32_t n, i;
	int found = 0;
	if(!f)
		return 0;
	if(fread(hdr, 1, 16, f) == 16 && memcmp(hdr, "BURIKO ARC20", 12) == 0)
	{
		n = hdr[12] | hdr[13] << 8 | hdr[14] << 16 | (uint32_t)hdr[15] << 24;
		for(i = 0; i < n && fread(e, 1, 0x80, f) == 0x80; i++)
			if(strcasecmp((const char*)e, name) == 0)
			{
				*off = 16 + n * 0x80 + (e[0x60] | e[0x61] << 8 | e[0x62] << 16 | (uint32_t)e[0x63] << 24);
				*size = e[0x64] | e[0x65] << 8 | e[0x66] << 16 | (uint32_t)e[0x67] << 24;
				found = 1;
				break;
			}
	}
	fclose(f);
	return found;
}

static void SilentFill(void* user, int16_t* samples, int frames)
{
	(void)user;
	memset(samples, 0, (size_t)frames * 4);
}

// --movie: the player alone, a picture per second
static int MovieTest(const char* spec, double seconds)
{
	char arc[1024], *colon;
	uint32_t off, size;
	int32_t length = 0;
	int i, r;
	snprintf(arc, sizeof arc, "%s", spec);
	colon = strrchr(arc, ':');
	if(!colon)
		return 2;
	*colon = 0;
	if(!ArcFind(arc, colon + 1, &off, &size))
	{
		fprintf(stderr, "hosttest: %s not in %s\n", colon + 1, arc);
		return 1;
	}
	OS_Init();
	OS_AudioStart(44100, SilentFill, NULL);
	r = OS_MovieOpenRange(arc, off, size);
	fprintf(stderr, "hosttest: open %s at %u (%u bytes): %d\n", colon + 1, off, size, r);
	if(r != OS_MOVIE_OK && r != OS_MOVIE_AUDIO_NOT_RENDERED)
		return 1;
	if(!OS_MovieStart(0, 0, 1280, 960, 0, &length))
		return 1;
	fprintf(stderr, "hosttest: length %d ms\n", (int)length);
	for(i = 1; i <= (int)seconds; i++)
	{
		AppleHostMovieFrame_t m;
		int playing;
		usleep(1000000);
		if(AppleHost_LockMovieFrame(0, &m, &playing))
		{
			char path[64];
			uint32_t* copy = (uint32_t*)malloc((size_t)m.width * m.height * 4);
			CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
			CGContextRef ctx;
			CGImageRef img;
			CFURLRef url;
			CGImageDestinationRef dst;
			memcpy(copy, m.pixels, (size_t)m.width * m.height * 4);
			AppleHost_UnlockMovieFrame();
			ctx = CGBitmapContextCreate(copy, (size_t)m.width, (size_t)m.height, 8, (size_t)m.width * 4, rgb,
				kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
			img = CGBitmapContextCreateImage(ctx);
			snprintf(path, sizeof path, "movie%d.png", i);
			url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
			dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
			CGImageDestinationAddImage(dst, img, NULL);
			CGImageDestinationFinalize(dst);
			CFRelease(dst);
			CFRelease(url);
			CGImageRelease(img);
			CGContextRelease(ctx);
			CGColorSpaceRelease(rgb);
			free(copy);
			fprintf(stderr, "hosttest: %s (%dx%d), running %d\n", path, m.width, m.height, OS_MovieIsRunning());
		}
	}
	OS_MovieStop();
	OS_AudioStop();
	return 0;
}

int main(int argc, char** argv)
{
	AppleHostConfig_t cfg;
	const char* secs = Opt(argc, argv, "--seconds");
	const char* script = Opt(argc, argv, "--script");
	const char* shot = Opt(argc, argv, "--shot");
	double deadline, waitUntil = 0;
	if(argc < 2)
	{
		fprintf(stderr, "usage: %s GAME_DIR [--seconds=N] [--script=...] [--shot=FILE] [--overlay=DIR] [--font=FILE]"
		                " [--save=DIR] [--chinese] [--args=...]\n", argv[0]);
		return 2;
	}
	if(Opt(argc, argv, "--movie"))
		return MovieTest(Opt(argc, argv, "--movie"), secs ? atof(secs) : 5);
	memset(&cfg, 0, sizeof cfg);
	cfg.gameDir = argv[1];
	cfg.overlayDir = Opt(argc, argv, "--overlay");
	cfg.fontFile = Opt(argc, argv, "--font");
	cfg.saveDir = Opt(argc, argv, "--save");
	cfg.chinese = Opt(argc, argv, "--chinese") != NULL;
	cfg.engineArgs = Opt(argc, argv, "--args");
	AppleHost_SetEndHandler(OnEnd);
	AppleHost_SetMessageBoxHandler(OnMessageBox);
	if(!AppleHost_Start(&cfg))
	{
		fprintf(stderr, "hosttest: the engine did not start\n");
		return 1;
	}
	deadline = Now() + (secs ? atof(secs) : 30);
	while(Now() < deadline && gEnded == -1000)
	{
		if(script && *script && Now() >= waitUntil)
		{
			char word[64], arg[512];
			int n = 0, x, y;
			while(*script == ' ')
				script++;
			if(sscanf(script, "%63s%n", word, &n) == 1)
			{
				script += n;
				if(!strcmp(word, "wait") && sscanf(script, "%d%n", &x, &n) == 1)
					waitUntil = Now() + x, script += n;
				else if((!strcmp(word, "click") || !strcmp(word, "rclick")) && sscanf(script, "%d %d%n", &x, &y, &n) == 2)
				{
					int b = word[0] == 'r';
					AppleHost_PointerMove(x, y);
					AppleHost_PointerButton(b, 1, x, y);
					usleep(60000);
					AppleHost_PointerButton(b, 0, x, y);
					waitUntil = Now() + 0.1;
					script += n;
				}
				else if(!strcmp(word, "move") && sscanf(script, "%d %d%n", &x, &y, &n) == 2)
					AppleHost_PointerMove(x, y), script += n;
				else if(!strcmp(word, "key") && sscanf(script, "%i%n", &x, &n) == 1)
				{
					AppleHost_Key(x, 1);
					usleep(60000);
					AppleHost_Key(x, 0);
					script += n;
				}
				else if(!strcmp(word, "wheel") && sscanf(script, "%d%n", &x, &n) == 1)
					AppleHost_Wheel(x), script += n;
				else if(!strcmp(word, "shot") && sscanf(script, "%511s%n", arg, &n) == 1)
					WriteShot(arg), script += n;
				else
				{
					fprintf(stderr, "hosttest: bad script word \"%s\"\n", word);
					script = NULL;
				}
			}
		}
		usleep(10000);
	}
	if(shot)
		WriteShot(shot);
	if(gEnded != -1000)
		fprintf(stderr, "hosttest: the engine ended (%d)\n", gEnded);
	else
		fprintf(stderr, "hosttest: time is up\n");
	return 0;
}
