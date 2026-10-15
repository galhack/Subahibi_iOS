/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * xin.c - inject pointer and keyboard input into an X display through the
 *         XTest extension, for driving the engine from a script
 *         (tools/play.sh clicks through a game under a virtual display)
 *
 *   xin move X Y          move the pointer to the root-window position X, Y
 *   xin click [BUTTON]    press and release a button (default 1, the left one)
 *   xin key KEYSYM        press and release a key ("Return", "space", "a")
 *   xin keydown KEYSYM    press a key and leave it down
 *   xin keyup KEYSYM      release it
 *
 * The display is $DISPLAY.  Exit status 0 when the event was sent, 1 when
 * the display cannot be opened or no command is given, 2 for a command
 * that is not one of the above or lacks its arguments.  The XTest header
 * is not installed on the build machine, so the three prototypes are
 * declared by hand and libXtst.so.6 is linked directly.  No project header
 * is used: this is a stand-alone helper.
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// the XTest calls (X11/extensions/XTest.h): the last argument is a delay in milliseconds, 0 here
extern int XTestFakeMotionEvent(Display*, int, int, int, unsigned long);
extern int XTestFakeButtonEvent(Display*, unsigned int, int, unsigned long);
extern int XTestFakeKeyEvent(Display*, unsigned int, int, unsigned long);

int main(int argc, char** argv)
{
	Display* d = XOpenDisplay(NULL);
	if(!d || argc < 2)
		return 1;
	if(strcmp(argv[1], "move") == 0 && argc >= 4)
		XTestFakeMotionEvent(d, -1, atoi(argv[2]), atoi(argv[3]), 0); // screen -1: the current one
	else if(strcmp(argv[1], "click") == 0)
	{
		unsigned b = argc >= 3 ? (unsigned)atoi(argv[2]) : 1;
		XTestFakeButtonEvent(d, b, 1, 0);
		XFlush(d);
		usleep(60000); // 60 ms between press and release
		XTestFakeButtonEvent(d, b, 0, 0);
	}
	else if((strcmp(argv[1], "keydown") == 0 || strcmp(argv[1], "keyup") == 0) && argc >= 3)
	{
		KeySym ks = XStringToKeysym(argv[2]);
		unsigned kc = XKeysymToKeycode(d, ks);
		XTestFakeKeyEvent(d, kc, argv[1][3] == 'd', 0); // "keydown" presses, "keyup" releases
	}
	else if(strcmp(argv[1], "key") == 0 && argc >= 3)
	{
		KeySym ks = XStringToKeysym(argv[2]);
		unsigned kc = XKeysymToKeycode(d, ks);
		XTestFakeKeyEvent(d, kc, 1, 0);
		XFlush(d);
		usleep(60000);
		XTestFakeKeyEvent(d, kc, 0, 0);
	}
	else
		return 2;
	XFlush(d);
	XCloseDisplay(d);
	return 0;
}
