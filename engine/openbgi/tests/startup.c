/* The title script already handles a missing optional MakerLogo prelude.
 * Only that exact lookup may be hidden; the files themselves stay usable. */
#include "bgi/vm.h"
#include "bgi/sys.h"
#include "bgi/engine.h"
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

static uint32_t Exists(Thread_t* t, const char* arc, const char* name)
{
	strcpy((char*)t->data, arc);
	strcpy((char*)t->data + 64, name);
	Thread_Push(t, VM_TAG_DATA);
	Thread_Push(t, VM_TAG_DATA | 64);
	vm_optable_80[0x34](t);
	return Thread_Pop(t);
}

int main(void)
{
	char dir[] = "/tmp/subahibi-startup-XXXXXX";
	Thread_t* t;
	FILE* f;
	if(!mkdtemp(dir) || chdir(dir))
		return 1;
	f = fopen("MakerLogo", "wb");
	if(!f)
		return 1;
	fputs("synthetic intro", f);
	fclose(f);
	OS_Init();
	Engine_SelectProfile("1.647");
	Vm_TablesInit();
	SetBaseDir(NULL);
	gArcMgr = FileSet_New();
	t = Thread_New(16, 256, 256);
	gSkipMakerLogo = 0;
	CHECK(Exists(t, "data01xxx.arc", "MakerLogo") != 0);
	gSkipMakerLogo = 1;
	CHECK(Exists(t, "data01xxx.arc", "MakerLogo") == 0);
	CHECK(Exists(t, "other.arc", "MakerLogo") != 0);
	CHECK(FileExists("data01xxx.arc", "MakerLogo") != 0);
	Thread_Delete(t);
	FileSet_Delete(gArcMgr);
	gArcMgr = NULL;
	OS_Shutdown();
	unlink("MakerLogo");
	chdir("/");
	rmdir(dir);
	printf("startup: %d failures\n", fails);
	return fails != 0;
}
