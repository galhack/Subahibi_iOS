/* 90 4A takes front, back, style, level, link. Exercise the script ABI
 * with distinct level/link values, including the Jabberwocky II failure. */
#include "bgi/vm.h"
#include "bgi/gfx.h"
#include "bgi/gfx/background.h"
#include "bgi/gfx/bmpmgr.h"
#include "bgi/error.h"
#include "bgi/os.h"
#include <stdio.h>
#include <unistd.h>

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

static int Flip(Thread_t* t, int level, int link)
{
	BgiTryFrame_t f;
	t->sp = 0;
	Thread_Push(t, 128);
	Thread_Push(t, 130);
	Thread_Push(t, 0);
	Thread_Push(t, level);
	Thread_Push(t, link);
	if(BGI_TRY(f))
	{
		vm_optable_90[0x4a](t);
		BGI_END_TRY(f);
		return 1;
	}
	fprintf(stderr, "valid flip rejected: level=%d link=%d\n", level, link);
	return 0;
}

int main(void)
{
	char dir[] = "/tmp/subahibi-bgflip-XXXXXX";
	Thread_t* t;
	Bmp_t front, back, out;
	Rect_t rect = {0, 0, 3, 3};
	const int levels[] = {256, 128, 0};
	if(!mkdtemp(dir) || chdir(dir))
		return 1;
	setenv("BGI_MSGBOX_STDERR", "1", 1);
	OS_Init();
	Engine_SelectProfile("1.647");
	Vm_TablesInit();
	Gfx_Create(64, 1);
	CHECK(Gfx_Resize(gGfx, 4, 4, PM_RGB32, 0, 0, 0, 0));
	CHECK(BmpMgr_Create(gBmpMgr, 128, 4, 4, PM_RGB32));
	CHECK(BmpMgr_Create(gBmpMgr, 130, 4, 4, PM_RGB32));
	CHECK(BmpMgr_GetInfo(gBmpMgr, &front, 128));
	CHECK(BmpMgr_GetInfo(gBmpMgr, &back, 130));
	Bmp_Fill(&front, NULL, 0xff0000);
	Bmp_Fill(&back, NULL, 0x0000ff);
	CHECK(Bmp_Alloc(&out, 4, 4, PM_RGB32));
	t = Thread_New(16, 256, 256);
	t->code[0] = 0x90;
	t->code[1] = 0x4a;
	for(int link = 0; link <= 1; link++)
		for(int i = 0; i < 3; i++)
		{
			Bg11_t* bg;
			CHECK(Flip(t, levels[i], link));
			bg = (Bg11_t*)gGfx->background;
			CHECK(bg->bg.obj.vt->getLevel(&bg->bg.obj) == levels[i]);
			CHECK(bg->link == link);
			if(levels[i] == 0 || (levels[i] == 256 && link == 1))
			{
				CHECK(Background_Render(&bg->bg, &out, &rect));
				CHECK((*(uint32_t*)out.pixels & 0xffffff) == (levels[i] ? 0x0000ff : 0xff0000));
			}
		}
	Thread_Delete(t);
	Bmp_Free(&out);
	Gfx_Destroy();
	OS_Shutdown();
	unlink("BGIError.txt");
	chdir("/tmp");
	rmdir(dir);
	printf("bgflip: %d failures\n", fails);
	return fails != 0;
}
