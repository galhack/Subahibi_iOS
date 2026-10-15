/*
 * cbgcheck - decode every CompressedBG image of the given archives in
 * memory and report the ones that fail, with the total decoding time.
 * Development check for the version 2 decoder (engine/openbgi/src/core/cbg2.c).
 *
 *   cbgcheck ARCHIVE...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "bgi/codec.h"

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(int argc, char** argv)
{
	long images = 0, failures = 0, alphaOpaque = 0;
	double seconds = 0;
	int a;
	for(a = 1; a < argc; a++)
	{
		FILE* f = fopen(argv[a], "rb");
		uint8_t hdr[16];
		uint32_t n, i;
		uint8_t* table;
		if(!f || fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "BURIKO ARC20", 12))
		{
			fprintf(stderr, "%s: not an archive\n", argv[a]);
			if(f)
				fclose(f);
			continue;
		}
		n = rd32(hdr + 12);
		table = malloc((size_t)n * 0x80);
		if(fread(table, 0x80, n, f) != n)
		{
			fclose(f);
			free(table);
			continue;
		}
		for(i = 0; i < n; i++)
		{
			const uint8_t* e = table + (size_t)i * 0x80;
			uint32_t off = rd32(e + 0x60), size = rd32(e + 0x64);
			uint8_t* src = malloc(size + 16);
			uint8_t* dst;
			clock_t t0;
			uint32_t r;
			fseek(f, 16 + (long)n * 0x80 + off, SEEK_SET);
			if(fread(src, 1, size, f) != size || !BGI_IsCbg(src))
			{
				free(src);
				continue;
			}
			dst = malloc(BGI_CbgOutSize(src));
			t0 = clock();
			r = BGI_CbgDecodeSized(src, size, dst);
			seconds += (double)(clock() - t0) / CLOCKS_PER_SEC;
			images++;
			if(r)
			{
				failures++;
				printf("FAIL %s:%s r=%08x\n", argv[a], (const char*)e, r);
			}
			else if((rd32(dst + 4) & 0xffff) == 32)
			{
				// a 32 bpp picture whose alpha is 0xFF everywhere: suspicious for a sprite
				uint32_t w = dst[0] | dst[1] << 8, h = dst[2] | dst[3] << 8, k, opaque = 1;
				for(k = 0; k < w * h && opaque; k++)
					opaque = dst[16 + k * 4 + 3] == 0xff;
				alphaOpaque += opaque;
			}
			free(dst);
			free(src);
		}
		free(table);
		fclose(f);
	}
	printf("%ld images, %ld failures, %ld fully opaque 32 bpp, %.2f s decoding (%.2f ms each)\n", images, failures,
	       alphaOpaque, seconds, images ? seconds * 1000 / images : 0);
	return failures != 0;
}
