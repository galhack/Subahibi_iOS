/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * cbg2.c - version 2 of the "CompressedBG___" images (the header word at
 *          +0x2E is 2): a JPEG-like coding the later builds use for full
 *          screen pictures; interface in codec.h
 *
 * The algorithm follows GARbro's ImageCBG.cs (Copyright (C) 2015-2018 by
 * morkt, MIT licence, https://github.com/morkt/GARbro):
 *
 *   * the 0x80 encrypted bytes after the header (decrypted and checked like
 *     the weight table of version 1) are two 8x8 quantisation tables, one
 *     for luma and one for the two chroma channels
 *   * two Huffman weight tables follow in the clear, 0x10 leaves for the DC
 *     sizes and 0xB0 for the AC run/size codes, then (height/8 + 1) offsets
 *     of the rows of blocks and, for 32 bpp, of the alpha plane
 *   * each row of 8x8 blocks holds the DC differences of every block, then
 *     the zig-zag AC coefficients; the inverse DCT gives YCbCr (or grey)
 *   * the alpha plane is an LZ77 stream of single bytes and back references
 *
 * The decoded result is the same as version 1 produces: a RawImageHeader
 * followed by tightly packed rows of the declared depth, cropped to the
 * declared size (the coding works on sizes padded to 8).
 */
#include "bgi/codec.h"

// ---- bit reader, most significant bit first -----------------------------------------------

typedef struct MsbBits
{
	const uint8_t* p;
	const uint8_t* end;
	uint32_t bits;
	int cached; // valid low bits of `bits`
} MsbBits_t;

// the next `count` (0 .. 16) bits; -1 at the end of the input
static int Bits_Get(MsbBits_t* b, int count)
{
	while(b->cached < count)
	{
		if(b->p >= b->end)
			return -1;
		b->bits = (b->bits << 8) | *b->p++;
		b->cached += 8;
	}
	b->cached -= count;
	return (int)((b->bits >> b->cached) & ((1u << count) - 1u));
}

// a size-prefixed value: the sign is in the top bit of the `count` bits (clear = negative)
static int Bits_Signed(MsbBits_t* b, int count)
{
	int v;
	if(count == 0)
		return 0;
	v = Bits_Get(b, count);
	if(v < 0)
		return 0;
	if((v >> (count - 1)) == 0)
		v = (int)((~0u << count) | (uint32_t)v) + 1;
	return v;
}

// 7 bits per byte, low group first; -1 past the end or beyond 32 bits
static int ReadVarint(const uint8_t** pp, const uint8_t* end)
{
	const uint8_t* p = *pp;
	uint32_t v = 0;
	int shift = 0;
	uint8_t c;
	do
	{
		if(p >= end || shift >= 32)
			return -1;
		c = *p++;
		v |= (uint32_t)(c & 0x7f) << shift;
		shift += 7;
	} while(c & 0x80);
	*pp = p;
	return (int)v;
}

// ---- Huffman tree of version 2 ------------------------------------------------------------

typedef struct HNode
{
	int valid;
	int isParent;
	uint32_t weight;
	int left, right; // children of a parent; -1 for a missing one
} HNode_t;

typedef struct HTree
{
	HNode_t nodes[2 * 0xB0 + 2];
	int root;
} HTree_t;

/* Pair the two lightest valid nodes until one carries every weight.  Unlike
 * version 1 each search starts from the first valid node and, for the second
 * child, never compares index 0 / 1 again (the original's loop bounds). */
static int HTree_Build(HTree_t* t, const uint32_t* weights, int leaves)
{
	uint32_t total = 0;
	int count = leaves, i;
	for(i = 0; i < leaves; i++)
	{
		t->nodes[i].valid = weights[i] != 0;
		t->nodes[i].isParent = 0;
		t->nodes[i].weight = weights[i];
		t->nodes[i].left = t->nodes[i].right = -1;
		total += weights[i];
	}
	for(;;)
	{
		int child[2], k;
		uint32_t weight = 0;
		if(count >= (int)(sizeof t->nodes / sizeof t->nodes[0]))
			return 0;
		for(k = 0; k < 2; k++)
		{
			uint32_t best = 0xffffffffu;
			int n = 0;
			child[k] = -1;
			for(; n < count; n++)
				if(t->nodes[n].valid)
				{
					best = t->nodes[n].weight;
					child[k] = n++;
					break;
				}
			if(n < k + 1)
				n = k + 1;
			for(; n < count; n++)
				if(t->nodes[n].valid && t->nodes[n].weight < best)
				{
					best = t->nodes[n].weight;
					child[k] = n;
				}
			if(child[k] == -1)
				continue;
			t->nodes[child[k]].valid = 0;
			weight += t->nodes[child[k]].weight;
		}
		t->nodes[count].valid = 1;
		t->nodes[count].isParent = 1;
		t->nodes[count].weight = weight;
		t->nodes[count].left = child[0];
		t->nodes[count].right = child[1];
		count++;
		if(weight >= total)
			break;
	}
	t->root = count - 1;
	return 1;
}

// the leaf index reached from the root; -1 at the end of the input or on a broken tree
static int HTree_Decode(const HTree_t* t, MsbBits_t* b)
{
	int node = t->root;
	do
	{
		int bit = Bits_Get(b, 1);
		if(bit < 0)
			return -1;
		node = bit ? t->nodes[node].right : t->nodes[node].left;
		if(node < 0)
			return -1;
	} while(t->nodes[node].isParent);
	return node;
}

// ---- the inverse DCT ----------------------------------------------------------------------

static const float kDctScale[64] = {
	1.00000000f, 1.38703990f, 1.30656302f, 1.17587554f, 1.00000000f, 0.78569496f, 0.54119611f, 0.27589938f,
	1.38703990f, 1.92387950f, 1.81225491f, 1.63098633f, 1.38703990f, 1.08979023f, 0.75066054f, 0.38268343f,
	1.30656302f, 1.81225491f, 1.70710683f, 1.53635550f, 1.30656302f, 1.02655995f, 0.70710677f, 0.36047992f,
	1.17587554f, 1.63098633f, 1.53635550f, 1.38268340f, 1.17587554f, 0.92387950f, 0.63637930f, 0.32442334f,
	1.00000000f, 1.38703990f, 1.30656302f, 1.17587554f, 1.00000000f, 0.78569496f, 0.54119611f, 0.27589938f,
	0.78569496f, 1.08979023f, 1.02655995f, 0.92387950f, 0.78569496f, 0.61731654f, 0.42521504f, 0.21677275f,
	0.54119611f, 0.75066054f, 0.70710677f, 0.63637930f, 0.54119611f, 0.42521504f, 0.29289323f, 0.14931567f,
	0.27589938f, 0.38268343f, 0.36047992f, 0.32442334f, 0.27589938f, 0.21677275f, 0.14931567f, 0.07612047f,
};

static const uint8_t kZigzag[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static uint8_t ToSample(float f)
{
	int a = 0x80 + (((int)f) >> 3);
	if(a <= 0)
		return 0;
	if(a <= 0xff)
		return (uint8_t)a;
	if(a < 0x180)
		return 0xff;
	return 0;
}

static uint8_t ToByte(float f)
{
	if(f >= 255.0f)
		return 0xff;
	if(f <= 0.0f)
		return 0;
	return (uint8_t)f;
}

// one 8x8 block of coefficients `c` with the quantisation table `q` into 64 samples
static void InverseDct(const int16_t* c, const float* q, uint8_t* out)
{
	float tmp[8][8];
	int i;
	for(i = 0; i < 8; i++)
	{
		float v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17;
		if(!c[8 + i] && !c[16 + i] && !c[24 + i] && !c[32 + i] && !c[40 + i] && !c[48 + i] && !c[56 + i])
		{
			float t = c[i] * q[i];
			int r;
			for(r = 0; r < 8; r++)
				tmp[r][i] = t;
			continue;
		}
		v1 = c[i] * q[i];
		v2 = c[8 + i] * q[8 + i];
		v3 = c[16 + i] * q[16 + i];
		v4 = c[24 + i] * q[24 + i];
		v5 = c[32 + i] * q[32 + i];
		v6 = c[40 + i] * q[40 + i];
		v7 = c[48 + i] * q[48 + i];
		v8 = c[56 + i] * q[56 + i];

		v10 = v1 + v5;
		v11 = v1 - v5;
		v12 = v3 + v7;
		v13 = (v3 - v7) * 1.414213562f - v12;
		v1 = v10 + v12;
		v7 = v10 - v12;
		v3 = v11 + v13;
		v5 = v11 - v13;
		v14 = v2 + v8;
		v15 = v2 - v8;
		v16 = v6 + v4;
		v17 = v6 - v4;
		v8 = v14 + v16;
		v11 = (v14 - v16) * 1.414213562f;
		v9 = (v17 + v15) * 1.847759065f;
		v10 = 1.082392200f * v15 - v9;
		v13 = -2.613125930f * v17 + v9;
		v6 = v13 - v8;
		v4 = v11 - v6;
		v2 = v10 + v4;

		tmp[0][i] = v1 + v8;
		tmp[1][i] = v3 + v6;
		tmp[2][i] = v5 + v4;
		tmp[3][i] = v7 - v2;
		tmp[4][i] = v7 + v2;
		tmp[5][i] = v5 - v4;
		tmp[6][i] = v3 - v6;
		tmp[7][i] = v1 - v8;
	}
	for(i = 0; i < 8; i++)
	{
		float v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17;
		uint8_t* o = out + i * 8;
		v10 = tmp[i][0] + tmp[i][4];
		v11 = tmp[i][0] - tmp[i][4];
		v12 = tmp[i][2] + tmp[i][6];
		v13 = tmp[i][2] - tmp[i][6];
		v14 = tmp[i][1] + tmp[i][7];
		v15 = tmp[i][1] - tmp[i][7];
		v16 = tmp[i][5] + tmp[i][3];
		v17 = tmp[i][5] - tmp[i][3];

		v13 = 1.414213562f * v13 - v12;
		v1 = v10 + v12;
		v7 = v10 - v12;
		v3 = v11 + v13;
		v5 = v11 - v13;
		v8 = v14 + v16;
		v11 = (v14 - v16) * 1.414213562f;
		v9 = (v17 + v15) * 1.847759065f;
		v10 = v9 - v15 * 1.082392200f;
		v13 = v9 - v17 * 2.613125930f;
		v6 = v13 - v8;
		v4 = v11 - v6;
		v2 = v10 - v4;

		o[0] = ToSample(v1 + v8);
		o[1] = ToSample(v3 + v6);
		o[2] = ToSample(v5 + v4);
		o[3] = ToSample(v7 + v2);
		o[4] = ToSample(v7 - v2);
		o[5] = ToSample(v5 - v4);
		o[6] = ToSample(v3 - v6);
		o[7] = ToSample(v1 - v8);
	}
}

// ---- the decoder --------------------------------------------------------------------------

typedef struct Cbg2
{
	float quant[2][64];
	HTree_t dcTree, acTree;
	int w, h;     // the coded size, padded to 8
	int channels; // 1 (8 bpp) or 3
	uint8_t* bgra; // w * h * 4, the decoded picture
} Cbg2_t;

/* One row of 8x8 blocks: the coefficients of every block (channel by
 * channel, w/8 blocks each) are decoded first, then turned into pixels at
 * row `by` of the picture. */
static void Cbg2_Row(Cbg2_t* d, const uint8_t* src, const uint8_t* end, int by, int16_t* coef)
{
	MsbBits_t b;
	int size, i, bx, blocks = d->w / 8;
	int acc = 0;
	size = ReadVarint(&src, end);
	if(size <= 0 || size > blocks * 64 * d->channels)
		return;
	memset(coef, 0, (size_t)blocks * 64 * d->channels * sizeof *coef);
	b.p = src;
	b.end = end;
	b.bits = 0;
	b.cached = 0;
	// the DC values, as differences to the previous block
	for(i = 0; i < size && b.p < b.end; i += 64)
	{
		int count = HTree_Decode(&d->dcTree, &b);
		if(count < 0)
			break;
		acc += Bits_Signed(&b, count);
		coef[i] = (int16_t)acc;
	}
	if(b.cached & 7)
		Bits_Get(&b, b.cached & 7);
	// the AC values: 4 bits of zero run, 4 bits of size; 0 ends the block, 0xF skips 16
	for(i = 0; i < size && b.p < b.end; i += 64)
	{
		int index = 1;
		while(index < 64 && b.p < b.end)
		{
			int code = HTree_Decode(&d->acTree, &b);
			if(code <= 0)
				break;
			if(code == 0xF)
			{
				index += 0x10;
				continue;
			}
			index += code & 0xF;
			if(index >= 64)
				break;
			coef[i + kZigzag[index]] = (int16_t)Bits_Signed(&b, code >> 4);
			index++;
		}
	}
	for(bx = 0; bx < blocks; bx++)
	{
		uint8_t sample[3][64];
		int c, j;
		for(c = 0; c < d->channels; c++)
			InverseDct(coef + c * d->w * 8 + bx * 64, d->quant[c > 0], sample[c]);
		for(j = 0; j < 64; j++)
		{
			uint8_t* px = d->bgra + (((size_t)by * 8 + (j >> 3)) * d->w + (size_t)bx * 8 + (j & 7)) * 4;
			if(d->channels == 1)
			{
				px[0] = px[1] = px[2] = sample[0][j];
				continue;
			}
			{
				// full-range YCbCr to RGB
				float y = sample[0][j], cb = sample[1][j], cr = sample[2][j];
				px[0] = ToByte(y + 1.772f * cb - 226.316f);
				px[1] = ToByte(y - 0.34414f * cb - 0.71414f * cr + 135.95984f);
				px[2] = ToByte(y + 1.402f * cr - 178.956f);
			}
		}
	}
}

/* The alpha plane: a flag byte per 8 items; a set flag is a 16-bit back
 * reference (dx 6 bits, dy 3 bits, both signed and dy never positive, the
 * count less 3 in 7 bits), a clear one a literal byte.  0 when the stream is
 * not an alpha plane or breaks off. */
static int Cbg2_Alpha(Cbg2_t* d, const uint8_t* src, const uint8_t* end)
{
	size_t total = (size_t)d->w * d->h * 4, dst = 3;
	uint32_t ctl = 2;
	if(end - src < 4 || src[0] != 1 || src[1] || src[2] || src[3])
		return 0;
	src += 4;
	while(dst < total)
	{
		ctl >>= 1;
		if(ctl == 1)
		{
			if(src >= end)
				return 0;
			ctl = *src++ | 0x100u;
		}
		if(ctl & 1)
		{
			int v, x, y, count, k;
			long from;
			if(end - src < 2)
				return 0;
			v = src[0] | src[1] << 8;
			src += 2;
			x = v & 0x3f;
			if(x > 0x1f)
				x -= 0x40;
			y = (v >> 6) & 7;
			if(y)
				y -= 8;
			count = ((v >> 9) & 0x7f) + 3;
			from = (long)dst + ((long)x + (long)y * d->w) * 4;
			if(from < 0 || (size_t)from >= dst)
				return 0;
			for(k = 0; k < count && dst < total; k++)
			{
				d->bgra[dst] = d->bgra[from];
				from += 4;
				dst += 4;
			}
		}
		else
		{
			if(src >= end)
				return 0;
			d->bgra[dst] = *src++;
			dst += 4;
		}
	}
	return 1;
}

// the encrypted quantisation tables: decrypt, check, scale; 0 when the checks fail
static int Cbg2_Tables(Cbg2_t* d, const CbgHeader_t* h, const uint8_t* enc)
{
	uint8_t q[0x80];
	uint32_t key = h->key;
	uint8_t sum = 0, x = 0;
	int i;
	for(i = 0; i < 0x80; i++)
	{
		q[i] = (uint8_t)(enc[i] - (uint8_t)BGI_HashUpdate(&key));
		sum = (uint8_t)(sum + q[i]);
		x ^= q[i];
	}
	// the checks cover the whole encrypted block; it is 0x80 bytes in every known file
	for(; (uint32_t)i < h->encLen; i++)
	{
		uint8_t c = (uint8_t)(enc[i] - (uint8_t)BGI_HashUpdate(&key));
		sum = (uint8_t)(sum + c);
		x ^= c;
	}
	if(sum != h->sum || x != h->xor)
		return 0;
	for(i = 0; i < 0x80; i++)
		d->quant[i >> 6][i & 0x3f] = q[i] * kDctScale[i & 0x3f];
	return 1;
}

uint32_t BGI_CbgDecodeV2(const void* src, uint32_t srcLen, void* dst)
{
	const CbgHeader_t* h = (const CbgHeader_t*)src;
	const uint8_t* s = (const uint8_t*)src;
	/* every read is bounded by the file size; a caller that does not know it
	 * (0xFFFFFFFF) gets the original's trust in the offsets, up to 2 GB */
	size_t size = srcLen == 0xffffffffu ? 0x7fffffffu : srcLen;
	const uint8_t *end = s + size, *base, *p, *data;
	uint32_t w1[0x10], w2[0xB0];
	uint32_t bpp = h->bpp & 0xffff, ok = 0;
	int32_t* offs = NULL;
	int16_t* coef;
	size_t avail; // the bytes from `data` to the end of the file
	Cbg2_t* d;
	int i, rows, x, y;
	uint8_t* out = (uint8_t*)dst + 0x10;

	if(size < 0x30 || h->encLen < 0x80 || h->encLen > size - 0x30 || (bpp != 8 && bpp != 24 && bpp != 32))
		return 0x80000004u;
	d = (Cbg2_t*)BGI_Alloc(sizeof *d);
	memset(d, 0, sizeof *d);
	if(!Cbg2_Tables(d, h, s + 0x30))
	{
		BGI_Free(d);
		return 0x80000004u;
	}
	memcpy(dst, s + 0x10, 0x10);

	base = p = s + 0x30 + h->encLen;
	for(i = 0; i < 0x10; i++)
	{
		int v = ReadVarint(&p, end);
		if(v < 0)
			goto done;
		w1[i] = (uint32_t)v;
	}
	for(i = 0; i < 0xB0; i++)
	{
		int v = ReadVarint(&p, end);
		if(v < 0)
			goto done;
		w2[i] = (uint32_t)v;
	}
	d->w = (h->width + 7) & ~7;
	d->h = (h->height + 7) & ~7;
	d->channels = bpp == 8 ? 1 : 3;
	rows = d->h / 8;
	if(rows == 0 || !HTree_Build(&d->dcTree, w1, 0x10) || !HTree_Build(&d->acTree, w2, 0xB0))
		goto done;

	// the row offsets count from `base`; the rows themselves follow the table
	if((size_t)(end - p) < (size_t)(rows + 1) * 4)
		goto done;
	offs = (int32_t*)BGI_Alloc((size_t)(rows + 1) * sizeof *offs);
	for(i = 0; i <= rows; i++)
		offs[i] = (int32_t)(p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24);
	data = p + (rows + 1) * 4;
	avail = (size_t)(end - data);
	for(i = 0; i <= rows; i++)
	{
		// relative to `data`; anything outside the file becomes -1 (skipped)
		int64_t o = (int64_t)offs[i] - (int64_t)(data - base);
		offs[i] = o < 0 || (uint64_t)o > avail ? -1 : (int32_t)o;
	}

	d->bgra = (uint8_t*)BGI_Alloc((size_t)d->w * d->h * 4);
	memset(d->bgra, bpp == 32 ? 0xff : 0, (size_t)d->w * d->h * 4);
	coef = (int16_t*)BGI_Alloc((size_t)d->w * 8 * d->channels * sizeof *coef);
	{
		size_t padSkip = (size_t)(((d->w >> 3) + 7) >> 3); // a bit field of the blocks, unused by the decoder
		for(y = 0; y < rows; y++)
		{
			// a row ends where the next one (or the alpha plane) starts, the last one at the end of the file
			int32_t next = y + 1 < rows ? offs[y + 1] : (bpp == 32 ? offs[rows] : -1);
			size_t rowEnd = next >= offs[y] && next >= 0 ? (size_t)next : avail;
			if(offs[y] < 0 || (size_t)offs[y] + padSkip > rowEnd)
				continue;
			Cbg2_Row(d, data + offs[y] + padSkip, data + rowEnd, y, coef);
		}
	}
	if(bpp == 32 && offs[rows] >= 0)
		Cbg2_Alpha(d, data + offs[rows], end);
	BGI_Free(coef);

	// crop the padded picture into the tightly packed rows of the raw image
	for(y = 0; y < h->height; y++)
	{
		const uint8_t* row = d->bgra + (size_t)y * d->w * 4;
		for(x = 0; x < h->width; x++)
		{
			const uint8_t* px = row + x * 4;
			switch(bpp)
			{
				case 8: *out++ = px[0]; break;
				case 24:
					*out++ = px[0];
					*out++ = px[1];
					*out++ = px[2];
					break;
				default:
					memcpy(out, px, 4);
					out += 4;
					break;
			}
		}
	}
	BGI_Free(d->bgra);
	ok = 1;
done:
	if(offs)
		BGI_Free(offs);
	BGI_Free(d);
	return ok ? 0 : 0x80000004u;
}
