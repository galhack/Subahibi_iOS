/*
 * cbgtrunc - decode one CompressedBG file cut off at every 1/64 of its size
 * (and with random bytes overwritten) to check that the version 2 decoder
 * stays inside its buffers.  Run it under the address sanitizer.
 *
 *   cbgtrunc FILE
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bgi/codec.h"

// the engine's allocator, for this stand-alone check
void* BGI_Alloc(size_t n) { return malloc(n ? n : 1); }
void BGI_Free(void* p) { free(p); }

int main(int argc, char** argv)
{
	FILE* f = fopen(argv[1], "rb");
	long size;
	uint8_t *src, *copy, *dst;
	int i, fails = 0;
	if(!f)
		return 2;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	src = malloc((size_t)size);
	if(fread(src, 1, (size_t)size, f) != (size_t)size)
		return 2;
	fclose(f);
	dst = malloc(BGI_CbgOutSize(src));
	for(i = 1; i <= 64; i++)
	{
		long n = size * i / 64;
		if(n < 0x30)
			continue;
		copy = malloc((size_t)n); // exactly n bytes: the sanitizer sees any read past them
		memcpy(copy, src, (size_t)n);
		if(BGI_CbgDecodeSized(copy, (uint32_t)n, dst) != 0)
			fails++;
		free(copy);
	}
	srand(1);
	for(i = 0; i < 200; i++)
	{
		int k;
		copy = malloc((size_t)size);
		memcpy(copy, src, (size_t)size);
		for(k = 0; k < 16; k++)
			copy[0x30 + 0x80 + rand() % (size - 0xB0)] = (uint8_t)rand();
		BGI_CbgDecodeSized(copy, (uint32_t)size, dst);
		free(copy);
	}
	printf("truncated: %d of 64 refused; 200 corrupted copies decoded without a crash\n", fails);
	return 0;
}
