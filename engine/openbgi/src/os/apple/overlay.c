/* SPDX-License-Identifier: GPL-2.0-only */
#include "apple_internal.h"
#include <strings.h>
#include <sys/stat.h>

/* Supplemental translations cannot shadow the base game's title, scripts,
 * saves or font. The manifest is checked by the Swift host before starting. */
int AppleOverlay_FindSupplement(const char* folder, const char* name, char* out, size_t n)
{
	static const char* const allowed[] = {"extra", "8_af", "8-1_extra_01-1", "8-1_extra_01-2"};
	struct stat st;
	size_t i;
	int len;
	if(!folder || !folder[0] || !name)
		return 0;
	for(i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
		if(strcasecmp(name, allowed[i]) == 0)
		{
			len = snprintf(out, n, "%s/%s", folder, allowed[i]);
			return len >= 0 && (size_t)len < n && stat(out, &st) == 0 && S_ISREG(st.st_mode);
		}
	return 0;
}
