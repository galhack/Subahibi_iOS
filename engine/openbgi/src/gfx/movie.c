/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * movie.c - movie playback of "90 F0" .. "90 F3".  Interface: bgi/gfx.h
 *           (Movie_*, gMoviePlaying).
 *
 * The original plays a video file through a DirectShow filter graph shown
 * in a child window over the picture; the graph's event notifications
 * reach the main window procedure as WM_APP.  All of the COM work is in the
 * OS layer (OS_Movie*); this file keeps the engine-visible state: whether
 * a movie runs (the window then paints black and the display style cannot
 * be toggled), the volume, the error reporting and the length result.
 */
#include "bgi/gfx.h"
#include "bgi/gfx/gfxmgr.h"
#include "bgi/display.h"
#include "bgi/error.h"
#include "bgi/msg.h"
#include "bgi/os.h"
#include "bgi/file.h"

int gMoviePlaying;           // a movie is running: read by the window procedure and the display style toggle
static int gMovieGraph;      // a graph exists
static int gMovieLoaded;     // a file is rendered in it
static int32_t gMovieVolume; // IBasicAudio units, -10000 .. 0; applied to every movie started

// "90 F1" (and engine shutdown): release the graph, if any.  1 when there was one, else 0
int Movie_Stop(void)
{
	int had = gMovieGraph;
	if(!had)
		return 0;
	OS_MovieStop(); // IMediaControl::Stop when playing, hide and disown the video window, release all
	gMoviePlaying = 0;
	gMovieGraph = 0;
	gMovieLoaded = 0;
	return had;
}

/* Compare two Shift-JIS names ignoring the case of single-byte letters (the
 * archives store names as written, the loader compares them lower-cased);
 * the trail byte of a double-byte character is compared as it is. */
static int SjisCaseCmp(const char* a, const char* b)
{
	const uint8_t *p = (const uint8_t*)a, *q = (const uint8_t*)b;
	for(;; p++, q++)
	{
		int c = *p, d = *q;
		if(((c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc)) && c == d && p[1])
		{
			p++;
			q++;
			if(*p != *q)
				return *p - *q;
			continue;
		}
		if(c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if(d >= 'A' && d <= 'Z')
			d += 'a' - 'A';
		if(c != d || !c)
			return c - d;
	}
}

// the little-endian dword at p
static uint32_t Le32(const uint8_t* p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Look `name` up in the BURIKO ARC20 archive at `path` (an engine path):
 * 1 with the absolute offset and the size of the entry when found. */
static int ArcLocate(const char* path, const char* name, uint32_t* offset, uint32_t* size)
{
	OsFile_t* f = OS_FileOpenRead(path);
	uint8_t hdr[16], e[0x80];
	uint32_t n, i;
	int found = 0;
	if(!f)
		return 0;
	if(OS_FileRead(f, hdr, 16) == 16 && memcmp(hdr, "BURIKO ARC20", 12) == 0)
	{
		n = Le32(hdr + 12);
		for(i = 0; i < n && OS_FileRead(f, e, 0x80) == 0x80; i++)
		{
			e[0x5f] = 0;
			if(SjisCaseCmp((const char*)e, name) == 0)
			{
				*offset = 16 + n * 0x80 + Le32(e + 0x60);
				*size = Le32(e + 0x64);
				found = 1;
				break;
			}
		}
	}
	OS_FileClose(f);
	return found;
}

/* 1.647 plays the movies of its archives (SubarashikihibiHD keeps op.mpg
 * in data02101.arc): the archives of the base directory are searched in
 * name order for the entry.  1 with `arcPath`, offset and size when found. */
int Movie_FindInArchives(const char* name, char* arcPath, uint32_t* offset, uint32_t* size)
{
	char pattern[0x104], best[0x104] = "";
	OsFindData_t fd;
	OsFind_t* h;
	PathJoin(pattern, gBaseDir, "*.arc");
	h = OS_FindFirst(pattern, &fd);
	if(!h)
		return 0;
	do
	{
		char path[0x104];
		uint32_t off, len;
		if(fd.attrs & OS_ATTR_DIRECTORY)
			continue;
		PathJoin(path, gBaseDir, fd.name);
		// several archives may hold the name: the first in name order wins, whatever the listing order
		if((!best[0] || SjisCaseCmp(path, best) < 0) && ArcLocate(path, name, &off, &len))
		{
			strcpy(best, path);
			*offset = off;
			*size = len;
		}
	} while(OS_FindNext(h, &fd));
	OS_FindClose(h);
	if(!best[0])
		return 0;
	strcpy(arcPath, best);
	return 1;
}

/* "90 F0": stop whatever runs, build a graph for `path` and start it in
 * the (x, y, w, h) rectangle of the picture (mapped through the display
 * scaling) at the current volume.  A `size` other than 0 plays that many
 * bytes from `offset` of the file (a movie inside an archive).  1 when playback started, *outLength
 * then holding the length in ms as the player reports it.  A missing
 * audio device or renderer is a script error (thrown; the graph is kept
 * for the next stop); any other failure returns 0. */
int Movie_Play(int32_t* outLength, const char* path, uint32_t offset, uint32_t size, int x, int y, int w, int h)
{
	Bmp_t scaled;
	Rect_t r, m;
	int32_t volume = gMovieVolume;

	Movie_Stop();
	// build the graph with its interfaces and render the file into it (IGraphBuilder::RenderFile)
	switch(size ? OS_MovieOpenRange(path, offset, size) : OS_MovieOpen(path))
	{
		case OS_MOVIE_OK: gMovieGraph = 1; break;
		case OS_MOVIE_NO_AUDIO_DEVICE:
			gMovieGraph = 1;
			ThrowScriptError(MSG_MOVIE_NO_AUDIO_DEVICE);
			return 0;
		case OS_MOVIE_VIDEO_NOT_RENDERED:
			gMovieGraph = 1;
			ThrowScriptError(MSG_MOVIE_VIDEO_NOT_RENDERED);
			return 0;
		case OS_MOVIE_AUDIO_NOT_RENDERED:
			gMovieGraph = 1;
			ThrowScriptError(MSG_MOVIE_AUDIO_NOT_RENDERED);
			return 0;
		default: return 0; // the original keeps a failed graph until the next stop; the OS layer releases it at once
	}
	r.l = x;
	r.t = y;
	r.r = x + w - 1;
	r.b = y + h - 1;
	if(Gfx_GetScaledBmp(gGfx, &scaled)) // a scaled display: map the picture rectangle to window coordinates
	{
		Gfx_MapRectScaled(gGfx, &m, &r);
		x = m.l;
		y = m.t;
		w = m.r - m.l + 1;
		h = m.b - m.t + 1;
	}
	if(!OS_MovieStart(x, y, w, h, volume, outLength))
		return 0;
	gMovieLoaded = 1;
	gMoviePlaying = 1;
	return 1;
}

// "90 F2": a movie was started and the player still runs it
int Movie_IsPlaying(void)
{
	return gMoviePlaying && OS_MovieIsRunning();
}

/* "90 F3": the volume, 0 .. 0x80, for the running movie and the ones to
 * come; 0 is silence (-10000), otherwise the attenuation
 * -(12800 - 100 v) * 0.375 in hundredths of a decibel.  1 ok, 0 for a
 * value above 0x80 (nothing changes). */
int Movie_SetVolume(uint32_t v)
{
	int32_t vol;
	if(v > 0x80)
		return 0;
	if(v == 0)
		vol = -10000;
	else
		vol = (int32_t) - ((double)(int64_t)(0x3200 - (int32_t)(v * 100)) * 0.375000000009375); // the original's constant: 0.375 with rounding noise
	OS_MovieSetVolume(vol);                                                                     // the original applies it only when a graph exists; the OS layer ignores the call without one
	gMovieVolume = vol;
	return 1;
}

/* the WM_APP notification of the main window: the OS layer has drained
 * the graph's events, `completed` says the movie reached its end.  A
 * movie that is over (or was stopped meanwhile) is released and the
 * picture restored. */
void Movie_OnEvent(int completed)
{
	if(completed)
		gMoviePlaying = 0;
	if(!gMoviePlaying)
	{
		Movie_Stop();
		Window_Repaint();
	}
}
