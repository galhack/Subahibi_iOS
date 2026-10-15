/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * patterns.c - the particle patterns: the global parameter tables of
 *              "C0 25" / "C0 2D", the per-pattern bitmaps at 32 scales
 *              ("C0 24" / "C0 2C"), the animation frames of 1.494 on
 *              ("C0 18") and the random generator of the engine.
 *              Interface: particle_internal.h (and, for the instruction
 *              handlers, inc/bgi/gfx/screens.h).
 *
 * The patterns are shared by every particle screen.  A pattern of each
 * type has a parameter record (defined by its "C0 25" / "C0 2D"), up to 32
 * scaled copies of one bitmap (bound by "C0 24" / "C0 2C") and, from 1.494
 * on, optionally an animation that replaces the bitmap.
 */
#include "particle_internal.h"

Bmp_t gPatternBmpB[PTCL_PATTERNS][PTCL_SCALES]; // type 1
PatternB_t gPatternB[PTCL_PATTERNS];
Bmp_t gPatternBmpA[PTCL_PATTERNS][PTCL_SCALES]; // type 0
PatternA_t gPatternA[PTCL_PATTERNS];
int gPatternsReady;
PatternAnim_t gPatternAnim[2][PTCL_PATTERNS]; // [type][pattern]

// free every scaled frame of an animation and reset it to "no animation"
static void PatternAnim_Free(PatternAnim_t* a)
{
	int k, i;
	for(k = 0; k < PTCL_SCALES; k++)
	{
		if(a->frames[k])
		{
			for(i = 0; i < a->count; i++)
				Bmp_Free(&a->frames[k][i]);
			BGI_Free(a->frames[k]);
		}
	}
	memset(a, 0, sizeof *a);
}

/* install `count` frames (all of one size, RGB32 or ARGB32) as the
 * animation, scaled to the 32 sizes; `rate1` and `rate2` are the 16.16
 * frame advance per tick and its random spread.  0 ok, 0x80000003 a frame
 * does not conform, 0x80000004 more than PTCL_FRAMES frames; a count of 0
 * (or no frames) drops the animation. */
static uint32_t PatternAnim_Set(PatternAnim_t* a, int count, const Bmp_t* frames, int32_t rate1, int32_t rate2)
{
	int k, i;
	if(count <= 0 || !frames)
	{
		PatternAnim_Free(a);
		return 0;
	}
	if(count > PTCL_FRAMES)
		return 0x80000004;
	if(frames[0].mode != PM_RGB32 && frames[0].mode != PM_ARGB32)
		return 0x80000003;
	for(i = 1; i < count; i++)
		if(frames[i].w != frames[0].w || frames[i].h != frames[0].h || frames[i].mode != frames[0].mode)
			return 0x80000003;
	PatternAnim_Free(a);
	a->count = count;
	a->rate1 = rate1;
	a->rate2 = rate2;
	for(k = 0; k < PTCL_SCALES; k++)
	{
		int32_t s = (0x200000 - k * 0x10000) / 32; // the scale in 16.16: 32/32 down to 1/32
		a->frames[k] = (Bmp_t*)BGI_Calloc((size_t)count * sizeof(Bmp_t));
		for(i = 0; i < count; i++)
		{
			Bmp_t* dst = &a->frames[k][i];
			if(Bmp_Alloc(dst, (int)(((uint32_t)frames[i].w * (uint32_t)s) >> 16),
				   (int)(((uint32_t)frames[i].h * (uint32_t)s) >> 16), frames[i].mode))
				Blit_Scale(dst, &frames[i], s, s);
		}
	}
	return 0;
}

/* "C0 18" (1.494 on): make `count` frames the animation of a pattern of a
 * type (see PatternAnim_Set for the frames and rates).  0 ok, 0x80000001
 * bad type, 0x80000002 bad pattern, 0x80000005 a frame does not conform,
 * 0x80000006 too many frames. */
uint32_t Pattern_SetAnimation(int type, int pattern, int count, const Bmp_t* frames, int32_t rate1, int32_t rate2)
{
	uint32_t r;
	if((uint32_t)type >= 2)
		return 0x80000001;
	if((uint32_t)pattern >= PtclPatterns())
		return 0x80000002;
	r = PatternAnim_Set(&gPatternAnim[type][pattern], count, frames, rate1, rate2);
	if(r == 0x80000003)
		return 0x80000005;
	if(r == 0x80000004)
		return 0x80000006;
	return r;
}

static BmpMgr_t* gPtclBmpMgr; // set at start-up by both setters below; never read (the patterns use gBmpMgr)

void Ptcl_SetBmpMgrPtr(BmpMgr_t* m)
{
	gPtclBmpMgr = m;
}

void Pattern_SetBmpMgrPtr(BmpMgr_t* m)
{
	gPtclBmpMgr = m;
}

/* the engine's random integer: a 30-bit draw modulo |n + 1|, i.e. 0 .. n
 * for n >= 0.  0 for n == -1, where the original divides by zero. */
int32_t Ptcl_Rnd(int32_t n)
{
	int32_t r = (BGI_Rand() << 15) | BGI_Rand();
	int32_t m = n + 1;
	if(m < 0)
		m = -m;
	return m ? r % m : 0;
}

// ---- the pattern bitmaps ------------------------------------------------------------------

// free the scaled copies of one pattern
static void Pattern_Free(Bmp_t row[PTCL_SCALES])
{
	int k;
	for(k = 0; k < PTCL_SCALES; k++)
		if(row[k].pixels)
		{
			BGI_Free(row[k].pixels);
			memset(&row[k], 0, sizeof row[k]);
		}
}

static void Pattern_FreeAll(Bmp_t table[PTCL_PATTERNS][PTCL_SCALES])
{
	int i;
	for(i = 0; i < PTCL_PATTERNS; i++)
		Pattern_Free(table[i]);
}

/* replace a pattern's 32 scaled copies by copies of `src` (scale 32/32 ..
 * 1/32 in 16.16); NULL frees them.  0 ok, 0x80000001 bad pattern,
 * 0x80000003 the source is not an RGB32 / ARGB32 bitmap. */
static uint32_t Pattern_SetBitmap(Bmp_t table[PTCL_PATTERNS][PTCL_SCALES], uint32_t pattern, const Bmp_t* src)
{
	int32_t s16;
	Bmp_t* dst;
	if(pattern >= PtclPatterns())
		return 0x80000001;
	if(!src)
	{
		Pattern_Free(table[pattern]);
		return 0;
	}
	if(src->mode != PM_RGB32 && src->mode != PM_ARGB32)
		return 0x80000003;
	dst = table[pattern];
	for(s16 = 0x200000; s16 > 0; s16 -= 0x10000, dst++)
	{
		int32_t s = s16 / 32; // 1.0 down to 1/32
		if(dst->pixels)
			BGI_Free(dst->pixels);
		if(Bmp_Alloc(dst, (int)(((uint32_t)src->w * (uint32_t)s) >> 16), (int)(((uint32_t)src->h * (uint32_t)s) >> 16),
			   src->mode))
			Blit_Scale(dst, src, s, s);
	}
	return 0;
}

/* Pattern_SetBitmap on the table of a type; the result codes are
 * renumbered for the "C0 24" / "C0 2C" handlers: 0 ok, 0x80000001 bad
 * type, 0x80000002 bad pattern, 0x80000005 unusable bitmap */
static uint32_t Pattern_SetBitmapByType(uint32_t type, uint32_t pattern, const Bmp_t* src)
{
	uint32_t r;
	if(type >= 2)
		return 0x80000001;
	r = Pattern_SetBitmap(type == 0 ? gPatternBmpA : gPatternBmpB, pattern, src);
	switch(r)
	{
		case 0: return 0;
		case 0x80000001: return 0x80000002;
		case 0x80000003: return 0x80000005;
		default: return r; // not reached: Pattern_SetBitmap has no other result
	}
}

// clear the type 0 tables; the type 1 tables are never reset (zero-initialised storage in the original as well)
static void Pattern_ResetAll(void)
{
	memset(gPatternA, 0, sizeof gPatternA);
	memset(gPatternBmpA, 0, sizeof gPatternBmpA);
}

// engine start-up: the type 0 tables start empty (once per process)
void Ptcl_InitGlobals(void)
{
	if(!gPatternsReady)
	{
		Pattern_ResetAll();
		gPatternsReady = 1;
	}
}

// engine shutdown: free every scaled copy and animation of both types
void Ptcl_FreeGlobals(void)
{
	int i;
	Pattern_FreeAll(gPatternBmpA);
	Pattern_FreeAll(gPatternBmpB);
	for(i = 0; i < PTCL_PATTERNS; i++)
	{
		PatternAnim_Free(&gPatternAnim[0][i]);
		PatternAnim_Free(&gPatternAnim[1][i]);
	}
}

/* the picture of a particle at a scale index (0 = full size, 31 = 1/32):
 * the current frame of its pattern's animation when one is set, else the
 * pattern's scaled copy from `table`; NULL when there is nothing to draw
 * or the index is out of range */
const Bmp_t* Pattern_Bitmap(Bmp_t table[PTCL_PATTERNS][PTCL_SCALES], const Particle_t* p, int sizeIndex)
{
	const PatternAnim_t* a = &gPatternAnim[p->type][p->pattern];
	if(sizeIndex < 0 || sizeIndex >= PTCL_SCALES)
		return NULL;
	if(a->count > 0)
	{
		const Bmp_t* b = &a->frames[sizeIndex][(uint32_t)(p->framePos >> 16) % (uint32_t)a->count];
		return b->pixels ? b : NULL;
	}
	if(!table[p->pattern][sizeIndex].pixels)
		return NULL;
	return &table[p->pattern][sizeIndex];
}

// at birth: frame 0, and a frame rate of rate1 + Rnd(rate2) (0 without an animation)
void Particle_StartFrames(Particle_t* p)
{
	const PatternAnim_t* a = &gPatternAnim[p->type][p->pattern];
	p->framePos = 0;
	p->frameRate = a->count > 0 ? a->rate1 + Ptcl_Rnd(a->rate2) : 0;
}

// one tick: advance the 16.16 frame position, wrapping at the frame count
void Particle_StepFrames(Particle_t* p)
{
	const PatternAnim_t* a = &gPatternAnim[p->type][p->pattern];
	if(a->count <= 0)
		return;
	p->framePos += p->frameRate;
	while((uint32_t)p->framePos >= ((uint32_t)a->count << 16))
		p->framePos -= (int32_t)((uint32_t)a->count << 16);
}

// ---- the pattern parameters ("C0 25" / "C0 2D") ------------------------------------------

static int32_t Abs8(int32_t v) // |v| >> 8: how the setters store the spreads and variances
{
	return (v < 0 ? -v : v) >> 8;
}

/* "C0 25": the parameters of a type 0 pattern (16.16 values, stored as
 * 24.8; the spreads and variances as magnitudes).  1 ok, 0 for a pattern
 * outside the generation's count. */
int PatternA_Set(int pattern, int32_t xSpread, int32_t yStart, int32_t zSpread, int32_t vxBase, int32_t vxVar,
	int32_t vyBase, int32_t vyVar, int32_t vzBase, int32_t vzVar, int32_t windWeight)
{
	PatternA_t* d;
	if((uint32_t)pattern >= PtclPatterns())
		return 0;
	d = &gPatternA[pattern];
	d->defined = 1;
	d->xSpread = Abs8(xSpread);
	d->yStart = yStart >> 8;
	d->zSpread = Abs8(zSpread);
	d->vxBase = vxBase >> 8;
	d->vxVar = Abs8(vxVar);
	d->vyBase = vyBase >> 8;
	d->vyVar = Abs8(vyVar);
	d->vzBase = vzBase >> 8;
	d->vzVar = Abs8(vzVar);
	d->windWeight = windWeight >> 8; // "C0 25" passes the wind weight by pointer; the handler dereferences it
	return 1;
}

/* "C0 2D": the parameters of a type 1 pattern.  The positions, velocities
 * and the wind weight are 16.16 (stored as 24.8, the spreads and variances
 * as magnitudes); the life, stretch and fade values are ticks and stored as
 * given, except that a fade of 0 becomes 1 (it is a divisor).  1 ok, 0 for
 * a pattern outside the generation's count. */
int PatternB_Set(int pattern, int32_t lifeBase, int32_t lifeVar, int32_t xSpread, int32_t yStart, int32_t zSpread,
	int32_t vxBase, int32_t vxVar, int32_t vyBase, int32_t vyVar, int32_t vzBase, int32_t vzVar, int32_t turnBase,
	int32_t turnVar, int32_t windWeight, int32_t fadeIn, int32_t fadeOut, int32_t effect)
{
	PatternB_t* d;
	if((uint32_t)pattern >= PtclPatterns())
		return 0;
	d = &gPatternB[pattern];
	d->defined = 1;
	d->lifeBase = lifeBase;
	d->lifeVar = lifeVar;
	d->xSpread = Abs8(xSpread);
	d->yStart = yStart >> 8;
	d->zSpread = Abs8(zSpread);
	d->vxBase = vxBase >> 8;
	d->vxVar = Abs8(vxVar);
	d->vyBase = vyBase >> 8;
	d->vyVar = Abs8(vyVar);
	d->vzBase = vzBase >> 8;
	d->vzVar = Abs8(vzVar);
	d->turnBase = turnBase;
	d->turnVar = turnVar;
	d->fadeIn = fadeIn != 0 ? fadeIn : 1;
	d->fadeOut = fadeOut != 0 ? fadeOut : 1;
	d->effect = effect;
	d->windWeight = windWeight >> 8;
	return 1;
}

/* "C0 24" / "C0 2C": bind the managed bitmap `bmp` (-1 = none) to a
 * pattern of a type: its scaled copies replace the pattern's.  0 ok,
 * 0x80000001 bad type, 0x80000002 bad pattern, 0x80000004 no such bitmap,
 * 0x80000005 not an RGB32 / ARGB32 bitmap. */
static uint32_t Pattern_BindBitmap(int type, int pattern, int bmp)
{
	Bmp_t b;
	memset(&b, 0, sizeof b);
	if(bmp != -1 && !BmpMgr_GetInfo(gBmpMgr, &b, bmp))
		return 0x80000004;
	return Pattern_SetBitmapByType((uint32_t)type, (uint32_t)pattern, b.pixels ? &b : NULL);
}

uint32_t PatternA_BindBitmap(int pattern, int bmp) // "C0 24"
{
	return Pattern_BindBitmap(0, pattern, bmp);
}

uint32_t PatternB_BindBitmap(int pattern, int bmp) // "C0 2C"
{
	return Pattern_BindBitmap(1, pattern, bmp);
}
