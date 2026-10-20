/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Synthetic BMP fixtures exercise bottom-up rows, padding and unaligned
 * file buffers without any game assets.
 */
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/bmpmgr.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c)                                                 \
	do                                                           \
	{                                                            \
		if(!(c))                                                 \
		{                                                        \
			fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
			fails++;                                             \
		}                                                        \
	} while(0)

static void Put32(uint8_t* p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void DecodeRows(int depth, int offset)
{
	uint32_t storage[32] = {0};
	uint8_t* data = (uint8_t*)storage + offset;
	uint8_t expected[18], actual[18] = {0};
	int rowBytes = 3 * (depth / 8);
	int stride = (rowBytes + 3) & ~3;
	int x, y;
	Bmp_t bmp;

	data[0] = 'B';
	data[1] = 'M';
	Put32(data + 0xa, 54);
	Put32(data + 0xe, 40);
	Put32(data + 0x12, 3);
	Put32(data + 0x16, 2);
	data[0x1a] = 1;
	data[0x1c] = (uint8_t)depth;
	memset(data + 54, 0xee, (size_t)stride * 2);
	for(y = 0; y < 2; y++)
		for(x = 0; x < rowBytes; x++)
		{
			expected[y * rowBytes + x] = (uint8_t)(1 + y * rowBytes + x);
			data[54 + (1 - y) * stride + x] = expected[y * rowBytes + x];
		}
	CHECK(BmpOp_DecodeBmp(0, data) == 0);
	CHECK(BmpMgr_GetInfo(gBmpMgr, &bmp, 0));
	CHECK(bmp.w == 3 && bmp.h == 2);
	CHECK(BmpMgr_GetPixels(gBmpMgr, actual, 0, sizeof(actual), 0) == 0);
	CHECK(memcmp(actual, expected, (size_t)rowBytes * 2) == 0);
	BmpMgr_FreeAll(gBmpMgr);
}

int main(void)
{
	int offset;
	gBmpMgr = BmpMgr_New(1);
	for(offset = 0; offset < 4; offset++)
	{
		DecodeRows(8, offset);
		DecodeRows(24, offset);
	}
	BmpMgr_Delete(gBmpMgr);
	gBmpMgr = NULL;
	printf("bmpdecode: %d failures\n", fails);
	return fails != 0;
}
