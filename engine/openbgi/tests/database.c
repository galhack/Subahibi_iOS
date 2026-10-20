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
#include "bgi/engine.h"
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
	/* A surviving Kimika completion proves the first two chapters were
	 * reached, even if their menu bits were lost in an older release. */
	ClearGlobalMem();
	gGlobalMem[160 + 214 / 8] |= 1 << (214 % 8);
	CHECK(GlobalDB_Save());
	gRepairSubahibiProgress = 0;
	CHECK(GlobalDB_Load() == 0);
	CHECK((gGlobalMem[160 + 230 / 8] & (1 << (230 % 8))) == 0);
	gRepairSubahibiProgress = 1;
	CHECK(GlobalDB_Load() == 0);
	CHECK((gGlobalMem[160 + 230 / 8] & (1 << (230 % 8))) != 0);
	CHECK((gGlobalMem[160 + 231 / 8] & (1 << (231 % 8))) != 0);
	CHECK((gGlobalMem[160 + 232 / 8] & (1 << (232 % 8))) == 0);
	CHECK((gGlobalMem[160 + 214 / 8] & (1 << (214 % 8))) != 0);
	CHECK(GlobalDB_Save());
	ClearGlobalMem();
	CHECK(GlobalDB_Load() == 0);
	CHECK((gGlobalMem[160 + 231 / 8] & (1 << (231 % 8))) != 0);
	/* Fresh progress must not acquire any chapter unlocks. */
	ClearGlobalMem();
	CHECK(GlobalDB_Save());
	CHECK(GlobalDB_Load() == 0);
	CHECK(gGlobalMem[160 + 230 / 8] == 0);
	/* A main ending recovers its next entry even if an earlier menu bit
	 * survived; later existing entries also imply their prerequisites. */
	ClearGlobalMem();
	gGlobalMem[160 + 213 / 8] |= 1 << (213 % 8);
	gGlobalMem[160 + 230 / 8] |= 1 << (230 % 8);
	CHECK(GlobalDB_Save());
	CHECK(GlobalDB_Load() == 0);
	CHECK((gGlobalMem[160 + 232 / 8] & (1 << (232 % 8))) != 0);
	CHECK((gGlobalMem[160 + 233 / 8] & (1 << (233 % 8))) == 0);
	ClearGlobalMem();
	gGlobalMem[160 + 234 / 8] |= 1 << (234 % 8);
	gGlobalMem[160 + 247 / 8] |= 1 << (247 % 8);
	CHECK(GlobalDB_Save());
	CHECK(GlobalDB_Load() == 0);
	for(int flag = 230; flag <= 234; flag++)
		CHECK((gGlobalMem[160 + flag / 8] & (1 << (flag % 8))) != 0);
	CHECK((gGlobalMem[160 + 235 / 8] & (1 << (235 % 8))) == 0);
	CHECK((gGlobalMem[160 + 247 / 8] & (1 << (247 % 8))) != 0);
	gRepairSubahibiProgress = 0;
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
