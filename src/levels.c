/* SPDX-License-Identifier: GPL-3.0-or-later
 * VitaZoom - pinch-to-zoom kernel plugin for PS Vita (taiHEN)
 * Copyright (C) 2026 VitaZoom contributors
 */
/*
 * levels.c - zoom levels and the requested view. Pure logic (no kernel
 * calls), shared by the gestures, the menu and the display code, and built
 * on the host by the tests.
 */
#include <psp2kern/kernel/sysclib.h>
#include "vitazoom.h"

/* visible size in framebuffer pixels, ~4.6% apart, 960 down to 240 */
const int vz_lvl_w[VZ_LEVELS] = {
	960, 920, 880, 840, 800, 768, 720, 704, 672, 640, 616, 584, 560, 536, 512, 480,
	472, 448, 432, 408, 392, 376, 360, 344, 328, 312, 304, 288, 272, 264, 248, 240};
const int vz_lvl_h[VZ_LEVELS] = {
	544, 522, 498, 476, 454, 436, 408, 398, 380, 368, 350, 330, 318, 304, 290, 272,
	268, 254, 244, 232, 222, 214, 204, 194, 186, 176, 172, 164, 154, 150, 140, 136};
volatile uint32_t g_lvl_bad = 0;

volatile int g_level = 0;
volatile int g_x0 = 0, g_y0 = 0;

/* sizes listed by the SDK as supported: never marked bad */
int vz_lvl_documented(int l)
{
	return vz_lvl_w[l] == 720 || vz_lvl_w[l] == 640 || vz_lvl_w[l] == 480;
}

/* ---- helpers (relative to the current foreground framebuffer) ---- */

int vz_level_ok(int l)
{
	if (l == 0) return 1;
	if (l < 0 || l >= VZ_LEVELS || (g_lvl_bad & (1u << l))) return 0;
	if (vz_lvl_w[l] >= g_fb_w || vz_lvl_h[l] >= g_fb_h) return 0;
	/* viewport: origin limited to x <= 640, y <= 364, so the visible area
	 * must be at least 320x180 screen units to reach every edge (3x) */
	if (g_vp_mode && (vz_lvl_w[l] * SCREEN_W / g_fb_w < SCREEN_W - 640 ||
	                  vz_lvl_h[l] * SCREEN_H / g_fb_h < SCREEN_H - 364))
		return 0;
	return 1;
}

int vz_crop_w(int l) { return l > 0 ? vz_lvl_w[l] : g_fb_w; }
int vz_crop_h(int l) { return l > 0 ? vz_lvl_h[l] : g_fb_h; }
int vz_factor(int l) { return g_fb_w * 1000 / vz_crop_w(l); }

int vz_level_step(int l, int dir)
{
	for (int n = l + dir; n >= 0 && n < VZ_LEVELS; n += dir)
		if (vz_level_ok(n)) return n;
	return l;
}

int vz_max_level(void)
{
	for (int l = VZ_LEVELS - 1; l > 0; l--)
		if (vz_level_ok(l)) return l;
	return 0;
}

int vz_level_for_width(int w)
{
	int best = 0, bd = 1 << 30;
	for (int l = 1; l < VZ_LEVELS; l++) {
		if (!vz_level_ok(l)) continue;
		int d = vz_lvl_w[l] - w;
		if (d < 0) d = -d;
		if (d < bd) { bd = d; best = l; }
	}
	return best;
}

void vz_level_name(int l, char *buf, int len)
{
	int f = (vz_factor(l) + 5) / 10; /* x100 */
	snprintf(buf, len, "%d.%02dx", f / 100, f % 100);
}

void vz_set_view(int level, int x0, int y0)
{
	if (!vz_level_ok(level)) level = vz_level_for_width(vz_crop_w(level));
	int maxx = g_fb_w - vz_crop_w(level);
	int maxy = g_fb_h - vz_crop_h(level);
	if (x0 > maxx) x0 = maxx;
	if (y0 > maxy) y0 = maxy;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	g_x0 = x0;
	g_y0 = y0;
	g_level = level;
	g_redraw = 1;
}

