/* Chapter choices must remain visible without a mouse hover. Adapt only
 * the game's nine-entry chapter selector, leaving locked entries alone. */
#include "bgi/panel.h"
#include "bgi/vm.h"
#include "bgi/engine.h"
#include "bgi/os.h"
#include <stdio.h>

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
	const uint8_t module[] = {8, 0, 0, 0, 1, 0, 0, 0, 0};
	Thread_t* t;
	SpriteDescScript_t* d;
	SpriteGroupScript_t* g;
	SpriteButton_t* b;
	SpriteDesc_t* copy;
	OS_Init();
	Engine_SelectProfile("1.647");
	t = Thread_New(16, 256, 4096);
	Thread_LoadModule(t, module, "scrslct._bp");
	d = (SpriteDescScript_t*)t->data;
	g = (SpriteGroupScript_t*)(t->data + 64);
	b = (SpriteButton_t*)(t->data + 128);
	memset(t->data, 0, 4096);
	d->groupCount = 1;
	d->groups = VM_TAG_DATA | 64;
	g->count = 9;
	g->buttons = VM_TAG_DATA | 128;
	for(int i = 0; i < 9; i++)
	{
		b[i].bmpNormal = i * 3;
		b[i].bmpFocus = b[i].bmpSelected = i * 3 + 1;
		b[i].hitMaskBmp = i * 3 + 1;
	}
	b[0].enabled = b[0].shown = 1;
	gSubahibiChapterMenu = 0;
	CHECK(SpritePanel_CopyDesc(&copy, d, t) == 0);
	CHECK(copy->groups[0].buttons[0].bmpNormal == 0);
	SpritePanel_FreeDesc(copy);
	gSubahibiChapterMenu = 1;
	CHECK(SpritePanel_CopyDesc(&copy, d, t) == 0);
	CHECK(copy->groups[0].buttons[0].bmpNormal == 1);
	CHECK(copy->groups[0].buttons[0].hitMaskBmp == 1);
	CHECK(copy->groups[0].buttons[1].bmpNormal == 3);
	CHECK(copy->groups[0].buttons[1].enabled == 0);
	CHECK(b[0].bmpNormal == 0);
	SpritePanel_FreeDesc(copy);
	/* A different screen, even with nine buttons, must keep its originals. */
	Thread_FreeModules(t);
	Thread_LoadModule(t, module, "other._bp");
	CHECK(SpritePanel_CopyDesc(&copy, d, t) == 0);
	CHECK(copy->groups[0].buttons[0].bmpNormal == 0);
	SpritePanel_FreeDesc(copy);
	gSubahibiChapterMenu = 0;
	Thread_Delete(t);
	OS_Shutdown();
	printf("chaptermenu: %d failures\n", fails);
	return fails != 0;
}
