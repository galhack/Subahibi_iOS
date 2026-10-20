/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#ifdef BGI_APPLE
#include "../src/os/apple/apple_internal.h"
#include <unistd.h>

int main(void)
{
	char folder[] = "/tmp/openbgi-overlay-XXXXXX", path[1024], out[1024];
	FILE* f;
	assert(mkdtemp(folder));
	snprintf(path, sizeof path, "%s/extra", folder);
	f = fopen(path, "wb");
	assert(f);
	fclose(f);
	assert(AppleOverlay_FindSupplement(folder, "Extra", out, sizeof out));
	assert(strcmp(out, path) == 0);
	assert(!AppleOverlay_FindSupplement(folder, "8_af", out, sizeof out));
	assert(!AppleOverlay_FindSupplement(folder, "Main", out, sizeof out));
	assert(!AppleOverlay_FindSupplement(folder, "../extra", out, sizeof out));
	assert(!AppleOverlay_FindSupplement(folder, "UserData/extra", out, sizeof out));
	assert(!AppleOverlay_FindSupplement(folder, "Extra", out, 2));
	unlink(path);
	rmdir(folder);
	puts("apple supplemental overlay: passed");
	return 0;
}
#else
int main(void)
{
	return 0;
}
#endif
