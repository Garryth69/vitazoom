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

/* Picture area inside the framebuffer: apps that show a 4:3 picture (PS1
 * games, emulators) leave black bars around it, and the view is kept inside
 * the picture so panning to an edge never fills the screen with black. */
volatile int g_cont_x0, g_cont_y0, g_cont_x1 = SCREEN_W, g_cont_y1 = SCREEN_H;
volatile int g_cont_changed;
static int cand[4], cand_n;

void vz_content_reset(void)
{
	g_cont_x0 = g_cont_y0 = 0;
	g_cont_x1 = g_fb_w;
	g_cont_y1 = g_fb_h;
	cand_n = 0;
}

/* One measurement of the non-black area (exclusive x1/y1). A bigger area is
 * taken at once; a smaller one only after it was seen the same way 4 times
 * in a row (2 s), so a dark scene does not shrink it by accident. */
void vz_content_sample(int fb_w, int fb_h, int x0, int y0, int x1, int y1)
{
	if (fb_w != g_fb_w || fb_h != g_fb_h) return;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > fb_w) x1 = fb_w;
	if (y1 > fb_h) y1 = fb_h;
	/* all black, or too small to be the picture: no information */
	if ((x1 - x0) * 2 < fb_w || (y1 - y0) * 2 < fb_h) return;
	if (x0 < g_cont_x0 || y0 < g_cont_y0 || x1 > g_cont_x1 || y1 > g_cont_y1) {
		if (x0 > g_cont_x0) x0 = g_cont_x0;
		if (y0 > g_cont_y0) y0 = g_cont_y0;
		if (x1 < g_cont_x1) x1 = g_cont_x1;
		if (y1 < g_cont_y1) y1 = g_cont_y1;
		g_cont_x0 = x0; g_cont_y0 = y0; g_cont_x1 = x1; g_cont_y1 = y1;
		g_cont_changed = 1;
		cand_n = 0;
		return;
	}
	if (x0 == g_cont_x0 && y0 == g_cont_y0 && x1 == g_cont_x1 && y1 == g_cont_y1) {
		cand_n = 0;
		return;
	}
	int same = cand_n && cand[0] == x0 && cand[1] == y0 && cand[2] == x1 && cand[3] == y1;
	cand[0] = x0; cand[1] = y0; cand[2] = x1; cand[3] = y1;
	cand_n = same ? cand_n + 1 : 1;
	if (cand_n >= 4) {
		g_cont_x0 = x0; g_cont_y0 = y0; g_cont_x1 = x1; g_cont_y1 = y1;
		g_cont_changed = 1;
		cand_n = 0;
	}
}

/* keep [v, v+size) inside [lo, hi), or centered on it when it is bigger */
static int clamp_span(int v, int size, int lo, int hi, int max)
{
	if (hi - lo >= size) {
		if (v < lo) v = lo;
		if (v > hi - size) v = hi - size;
	} else {
		v = (lo + hi - size) / 2;
	}
	if (v > max) v = max;
	if (v < 0) v = 0;
	return v;
}

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
	int w = vz_crop_w(level), h = vz_crop_h(level);
	if (level > 0) {
		x0 = clamp_span(x0, w, g_cont_x0, g_cont_x1, g_fb_w - w);
		y0 = clamp_span(y0, h, g_cont_y0, g_cont_y1, g_fb_h - h);
	} else {
		x0 = y0 = 0;
	}
	g_x0 = x0;
	g_y0 = y0;
	g_level = level;
	g_redraw = 1;
}

