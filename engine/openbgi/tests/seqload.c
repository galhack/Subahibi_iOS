/* The native 90 F4 / 92 F1 ABI is &number, &info, archive, filename.
 * Exercise the actual opcode, rather than calling the wait constructor
 * directly (which cannot detect reversed script arguments). */
#include "bgi/bfmovie.h"
#include "bgi/gfx/bmseq.h"
#include "bgi/vm.h"
#include "bgi/sys.h"
#include "bgi/file.h"
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

int main(void)
{
	char dir[] = "/tmp/subahibi-seqload-XXXXXX";
	BfMovieHeader_t movie = {0};
	Thread_t* t;
	int32_t number, info[5];
	int variant;
	FILE* f;
	if(!mkdtemp(dir) || chdir(dir) != 0)
		return 1;
	memcpy(movie.magic, BFMOVIE_MAGIC, 16);
	movie.width = 2;
	movie.height = 2;
	movie.bpp = 32;
	movie.mode = 2;
	movie.fps = 3;
	movie.frames = 1;
	movie.offset[0] = sizeof(movie);
	f = fopen("clip.bm", "wb");
	if(!f)
		return 1;
	CHECK(fwrite(&movie, 1, sizeof(movie), f) == sizeof(movie));
	fclose(f);
	OS_Init();
	Engine_SelectProfile("1.647");
	Vm_TablesInit();
	SetBaseDir(NULL);
	gArcMgr = FileSet_New();
	for(variant = 0; variant < 3; variant++)
	{
		t = Thread_New(16, 256, 256);
		memset(t->data, 0, 256);
		strcpy((char*)t->data + 64, "clip.bm");
		strcpy((char*)t->data + 80, "0");
		Thread_Push(t, VM_TAG_DATA);
		Thread_Push(t, VM_TAG_DATA | 4);
		Thread_Push(t, VM_TAG_DATA | 80);
		Thread_Push(t, VM_TAG_DATA | 64);
		if(variant)
		{
			Thread_Push(t, variant - 1);
			CHECK(vm_optable_92[0xf1](t) == 2);
		}
		else
			CHECK(vm_optable_90[0xf4](t) == 2);
		CHECK(Thread_PollWait(t) == 1);
		CHECK(Thread_Pop(t) == 0);
		memcpy(&number, t->data, sizeof(number));
		memcpy(info, t->data + 4, sizeof(info));
		CHECK(number > 0);
		CHECK(info[0] == 2 && info[1] == 2 && info[2] == 2 && info[3] == 3 && info[4] == 1);
		BmSeq_FreeAll();
		Thread_Delete(t);
	}
	FileSet_Delete(gArcMgr);
	gArcMgr = NULL;
	OS_Shutdown();
	unlink("clip.bm");
	chdir("/tmp");
	rmdir(dir);
	printf("seqload: %d failures\n", fails);
	return fails != 0;
}
