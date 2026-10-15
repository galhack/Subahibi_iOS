/*
 * evbtest - extract the Enigma Virtual Box files of an executable with the
 * application's reader (app/Sources/Native/evb.c), for comparison with
 * evbunpack's output:
 *
 *   evbtest EXE OUT_DIR
 */
#include <stdio.h>
#include "evb.h"

static int Progress(void* user, uint64_t done, uint64_t total)
{
	(void)user;
	(void)done;
	(void)total;
	return 1;
}

int main(int argc, char** argv)
{
	char err[256] = "";
	int n;
	if(argc != 3)
		return 2;
	n = Evb_Extract(argv[1], argv[2], Progress, NULL, err, sizeof err);
	if(n < 0)
	{
		fprintf(stderr, "evbtest: %s\n", err);
		return 1;
	}
	printf("%d files\n", n);
	return 0;
}
