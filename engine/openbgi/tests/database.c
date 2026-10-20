/* Saved progress must be reloaded from the same redirected location it was
 * written to, even while a different BGI.gdb ships beside the game. */
#include "bgi/sysobj/database.h"
#include "bgi/sysobj/flags.h"
#include "bgi/sysobj/names.h"
#include "bgi/sys.h"
#include "bgi/file.h"
#include "bgi/os.h"
#include "bgi/os_posix.h"
#include "bgi/version.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

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
static char savePath[512];
static int Redirect(const char* rel, char* out, size_t n)
{
	if(strcmp(rel, "BGI.gdb"))
		return 0;
	snprintf(out, n, "%s", savePath);
	return 1;
}
int main(void)
{
	char dir[] = "/tmp/subahibi-database-XXXXXX";
	if(!mkdtemp(dir) || chdir(dir))
		return 1;
	OS_Init();
	Engine_SelectProfile("1.647");
	SetBaseDir(NULL);
	SetGlobalMemSize(4);
	gSysAreaSize = 0x100000;
	gSysArea = BGI_Calloc(gSysAreaSize);
	SysArea_Init();
	CHECK(GlobalDB_Save()); // original game's database: no chapter progress
	snprintf(savePath, sizeof savePath, "%s/player.gdb", dir);
	gPosixPathRedirect = Redirect;
	gGlobalMem[160 + 230 / 8] |= 1 << (230 % 8);
	gSysArea[17] = 91;
	GName_Add("earned-scene");
	FlagMgr_Register(gFlagMgr, "read-lines", 17);
	FlagMgr_SetBit(gFlagMgr, "read-lines", 16, 1);
	CHECK(GlobalDB_Save());
	ClearGlobalMem();
	memset(gSysArea, 0, gSysAreaSize);
	CHECK(GlobalDB_Load() == 0);
	CHECK((gGlobalMem[160 + 230 / 8] & (1 << (230 % 8))) != 0);
	CHECK(gSysArea[17] == 91);
	CHECK(GName_Exists("earned-scene"));
	CHECK(FlagMgr_Group(gFlagMgr, 0)->bits == 17);
	gPosixPathRedirect = NULL;
	SysArea_Shutdown();
	BGI_Free(gSysArea);
	gSysArea = NULL;
	BGI_Free(gGlobalMem);
	gGlobalMem = NULL;
	OS_Shutdown();
	unlink("BGI.gdb");
	unlink("player.gdb");
	chdir("/");
	rmdir(dir);
	printf("database: %d failures\n", fails);
	return fails != 0;
}
