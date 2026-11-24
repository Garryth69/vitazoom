/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test stubs: the state normally owned by main.c / zoom.c / hooks.c and
 * a file API rooted in a temporary directory. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <psp2kern/kernel/iofilemgr.h>
#include "vitazoom.h"

uint32_t test_now_us = 10000000;
char test_root[256];

volatile int g_armed, g_menu_open, g_block_buttons;
volatile vz_stats_t g_st;
SceUID g_worker_thid = -1;
volatile int g_shown_level, g_shown_x, g_shown_y, g_zoom_fail;
volatile SceUID g_fg_pid = -1, g_app_pid = -1, g_pspemu_pid = -1;
volatile int g_hide_rstick, g_exclusive_running;
volatile int g_fb_w = SCREEN_W, g_fb_h = SCREEN_H;
volatile int g_redraw;
volatile int g_vp_mode = 1, g_vp_active, g_vp_ret;
volatile int g_snap_on;
volatile SceUID g_self_peek_thid = -1;
int test_snap_available; /* what vz_snap_begin answers */

void vz_log(const char *fmt, ...) { (void)fmt; }
int vz_snap_begin(void) { g_snap_on = test_snap_available; return g_snap_on; }
void vz_snap_end(void) { g_snap_on = 0; }
int vz_snap_visible(void) { return g_snap_on; }
void vz_snap_poll(void) {}
void vz_view_off(void) {}

/* "ur0:data/vitazoom/x" -> "<root>/ur0/data/vitazoom/x" */
static void map(const char *p, char *out, size_t n)
{
	const char *c = strchr(p, ':');
	snprintf(out, n, "%s/%.*s/%s", test_root, c ? (int)(c - p) : 0, p, c ? c + 1 : p);
}

SceUID ksceIoOpen(const char *path, int flags, int mode)
{
	char p[512];
	map(path, p, sizeof(p));
	int f = 0;
	if (flags & SCE_O_WRONLY) f |= O_WRONLY; else f |= O_RDONLY;
	if (flags & SCE_O_CREAT) f |= O_CREAT;
	if (flags & SCE_O_TRUNC) f |= O_TRUNC;
	if (flags & SCE_O_APPEND) f |= O_APPEND;
	int fd = open(p, f, mode);
	return fd < 0 ? (SceUID)0x80010002 : fd;
}
int ksceIoRead(SceUID fd, void *buf, SceSize n) { return (int)read(fd, buf, n); }
int ksceIoWrite(SceUID fd, const void *buf, SceSize n) { return (int)write(fd, buf, n); }
int ksceIoClose(SceUID fd) { return close(fd); }
int ksceIoMkdir(const char *path, int mode)
{
	char p[512], tmp[512];
	map(path, p, sizeof(p));
	/* create parents too (the console has ur0:data already) */
	for (char *s = p + strlen(test_root) + 1; (s = strchr(s, '/')); s++) {
		snprintf(tmp, sizeof(tmp), "%.*s", (int)(s - p), p);
		mkdir(tmp, 0777);
	}
	return mkdir(p, mode);
}
int ksceIoRemove(const char *path)
{
	char p[512];
	map(path, p, sizeof(p));
	return unlink(p);
}
