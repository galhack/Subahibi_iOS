/* Regression for HD pictures larger than the legacy 32 MiB buffer.
 * Synthetic fixtures contain no game data. BGI_GAME_DIR optionally tests
 * the purchased game's compressed picture from the iPhone crash. */
#include "bgi/file.h"
#include "bgi/os.h"
#include "bgi/codec.h"
#include "bgi/version.h"
#include "bgi/waitobj.h"
#include "bgi/gfx/bmpops.h"
#include "bgi/gfx/bmpmgr.h"
#include <stdio.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if(!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while(0)

int main(void)
{
	char dir[] = "/tmp/subahibi-imageload-XXXXXX", path[512];
	const uint32_t size = 16 + 11766u * 992u * 4u;
	uint8_t* raw = calloc(1, size);
	RawImageHeader_t* h = (RawImageHeader_t*)raw;
	FILE* f;
	Bmp_t bmp;
	Wait_t* wait;
	const char* game = getenv("BGI_GAME_DIR");
	CHECK(raw != NULL);
	if(!raw || !mkdtemp(dir))
		return 1;
	Engine_SelectProfile("1.647");
	gBmpMgr = BmpMgr_New(2);
	h->width = 11766;
	h->height = 992;
	h->bpp = 32;
	raw[size - 1] = 123;
	snprintf(path, sizeof(path), "%s/large", dir);
	f = fopen(path, "wb");
	if(!f)
		return 1;
	CHECK(fwrite(raw, 1, size, f) == size);
	fclose(f);
	OS_Init();
	CHECK(OS_SetGameDir(dir));
	SetBaseDir("C:\\");
	CHECK(BmpOp_LoadFile(0, NULL, "large") == 0);
	CHECK(BmpMgr_GetInfo(gBmpMgr, &bmp, 0));
	CHECK(bmp.w == 11766 && bmp.h == 992);
	CHECK(bmp.pixels[(size_t)bmp.pitch * (bmp.h - 1) + bmp.w * 4 - 1] == 123);
	BmpMgr_FreeAll(gBmpMgr);
	wait = WaitBmpLoad_New(NULL, 0, "0", "large");
	CHECK(wait->vt->poll(wait) == 1);
	Wait_Release(wait);
	CHECK(BmpMgr_GetInfo(gBmpMgr, &bmp, 0));
	CHECK(bmp.w == 11766 && bmp.h == 992);
	BmpMgr_FreeAll(gBmpMgr);
	Loader_SetCacheSize(size * 2);
	CHECK(Loader_CacheAdd(NULL, "cached", raw, size));
	CHECK(BmpOp_LoadFile(0, NULL, "cached") == 0);
	CHECK(BmpMgr_GetInfo(gBmpMgr, &bmp, 0));
	CHECK(bmp.w == 11766 && bmp.h == 992);
	wait = WaitBmpLoad_New(NULL, 1, NULL, "cached");
	CHECK(wait->vt->poll(wait) == 1);
	Wait_Release(wait);
	CHECK(BmpMgr_GetInfo(gBmpMgr, &bmp, 1));
	CHECK(bmp.w == 11766 && bmp.h == 992);
	Loader_FreeCache();
	BmpMgr_FreeAll(gBmpMgr);
	free(raw);
	if(game)
	{
		static const char* names[] = {
			"sp0011a", "sp0011b", "sp0011c", "ev8001_yword", "ev8001_zword",
			"ev8010_bg_l", "guruguru", "schoolview", "sp2006a", "sp2006c"
		};
		unsigned i;
		CHECK(OS_SetGameDir(game));
		SetBaseDir("C:\\");
		gArcMgr = FileSet_New();
		for(i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		{
			const char* arc = i < 3 ? "data02940.arc" : "data02995.arc";
			printf("load %s:%s\n", arc, names[i]);
			CHECK(GetFileLoadCapacity(arc, names[i]) > 0x2000000u);
			CHECK(BmpOp_LoadFile(0, arc, names[i]) == 0);
			CHECK(BmpMgr_GetInfo(gBmpMgr, &bmp, 0));
			if(i < 3)
				CHECK(bmp.w == 11766 && bmp.h == 992);
			BmpMgr_FreeAll(gBmpMgr);
		}
		FileSet_Delete(gArcMgr);
		gArcMgr = NULL;
	}
	BmpMgr_Delete(gBmpMgr);
	gBmpMgr = NULL;
	unlink(path);
	chdir("/tmp");
	rmdir(dir);
	OS_Shutdown();
	printf("imageload: %d failures\n", fails);
	return fails != 0;
}
