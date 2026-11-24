/* SPDX-License-Identifier: GPL-3.0-or-later
 * VitaZoom - pinch-to-zoom kernel plugin for PS Vita (taiHEN)
 * Copyright (C) 2026 VitaZoom contributors
 */
/*
 * draw.c - overlay rendering without pixel buffers.
 *
 * The worker thread describes the menu/OSD as a small display list
 * (rectangles + pre-decoded glyph strings). The display hook, which runs in
 * the context of the process owning the framebuffer, rasterises one row at a
 * time (8-bit palette index: bg<<5 | fg<<2 | alpha), expands it to ABGR and
 * copies it into the framebuffer. Lists are double buffered so the worker can
 * build the next one while the current one is being drawn.
 */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include "vitazoom.h"
#include "font.h"

#include <psp2kern/kernel/threadmgr.h>

static volatile uint32_t osd_until; /* OSD shown until this time (us) */
volatile uint32_t g_menu_gen = 0;

#define DL_ITEMS 64
#define DL_CHARS 1536

enum { K_FILL, K_BIG, K_SMALL };

typedef struct {
	int16_t x, y, w, h;
	uint8_t kind, color;
	uint16_t off, len;
} item_t;

typedef struct {
	int w, h, n, nch;
	item_t it[DL_ITEMS];
	uint8_t ch[DL_CHARS]; /* glyph indices */
} dlist_t;

static dlist_t dls[2][2];
static volatile int front[2];
static dlist_t *cur;
static int cur_which;

static uint32_t palette[128];
static uint8_t rowidx[MENU_W];
static uint32_t rowbuf[MENU_W];
static volatile int blit_busy;

/* colors are 0xAABBGGRR */
static const uint32_t bg_col[4] = {0xFF1E1810, 0xFF6A4420, 0xFF2C2218, 0xFF141414};
static const uint32_t fg_col[8] = {
	0xFFE6E6E6, 0xFFFFFFFF, 0xFF9A9A9A, 0xFF00A5FF,
	0xFF80FF80, 0xFF5050FF, 0xFF00A5FF, 0xFFFFFFFF,
};

static uint32_t blend(uint32_t a, uint32_t b, int t /* 0..3 */)
{
	uint32_t r = 0xFF000000;
	for (int s = 0; s < 24; s += 8) {
		int ca = (a >> s) & 0xFF, cb = (b >> s) & 0xFF;
		r |= (uint32_t)((ca * (3 - t) + cb * t) / 3) << s;
	}
	return r;
}

static void init_palette(void)
{
	static int done;
	if (done) return;
	for (int bg = 0; bg < 4; bg++)
		for (int fg = 0; fg < 8; fg++)
			for (int a = 0; a < 4; a++)
				palette[(bg << 5) | (fg << 2) | a] = blend(bg_col[bg], fg_col[fg], a);
	done = 1;
}

/* ---- building ---- */

void vz_dl_begin(int which)
{
	init_palette();
	cur_which = which;
	cur = &dls[which][front[which] ^ 1];
	cur->n = cur->nch = 0;
	cur->w = which == DL_MENU ? MENU_W : OSD_W;
	cur->h = which == DL_MENU ? MENU_H : OSD_H;
}

void vz_dl_size(int w, int h)
{
	if (!cur) return;
	cur->w = w > MENU_W ? MENU_W : w;
	cur->h = h;
}

void vz_dl_commit(void)
{
	if (!cur) return;
	front[cur_which] ^= 1;
	if (cur_which == DL_MENU) g_menu_gen++;
	cur = NULL;
	g_redraw = 1;
}

void vz_fill(int x, int y, int w, int h, int bg)
{
	if (!cur || cur->n >= DL_ITEMS) return;
	item_t *it = &cur->it[cur->n++];
	it->x = x; it->y = y; it->w = w; it->h = h;
	it->kind = K_FILL; it->color = bg;
}

/* next code point from UTF-8 (only up to U+00FF matters) */
static int next_cp(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	int c = *s++;
	if (c >= 0xC0 && c < 0xE0 && (*s & 0xC0) == 0x80) {
		c = ((c & 0x1F) << 6) | (*s++ & 0x3F);
	} else if (c >= 0x80) {
		while ((*s & 0xC0) == 0x80) s++; /* skip unsupported sequence */
		c = '?';
	}
	*ps = (const char *)s;
	return c;
}

int vz_text_len(const char *s)
{
	int n = 0;
	while (*s) { next_cp(&s); n++; }
	return n;
}

static int add_text(int x, int y, const char *s, int fg, int big)
{
	if (!cur || cur->n >= DL_ITEMS) return x;
	item_t *it = &cur->it[cur->n];
	it->x = x; it->y = y;
	it->kind = big ? K_BIG : K_SMALL;
	it->color = fg;
	it->off = cur->nch;
	int len = 0;
	while (*s && cur->nch < DL_CHARS) {
		int c = next_cp(&s);
		int idx;
		if (big) {
			if (c >= 32 && c <= 126) idx = c - 32;
			else if (c >= 160 && c <= 255) idx = 95 + (c - 160);
			else idx = '?' - 32;
		} else {
			/* 8x16 font is ASCII only: drop accents (Latin-1 192..255) */
			static const char base[] =
				"AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
				"aaaaaaaceeeeiiiidnooooo/ouuuuyty";
			if (c >= 192 && c <= 255) c = base[c - 192];
			idx = (c >= 32 && c <= 126) ? c - 32 : '?' - 32;
		}
		cur->ch[cur->nch++] = (uint8_t)idx;
		len++;
	}
	it->len = len;
	it->w = len * (big ? BIG_W : FONT_W);
	it->h = big ? BIG_H : FONT_H;
	cur->n++;
	return x + it->w;
}

int vz_text_big(int x, int y, const char *s, int fg) { return add_text(x, y, s, fg, 1); }
int vz_text_small(int x, int y, const char *s, int fg) { return add_text(x, y, s, fg, 0); }

/* ---- rasterising ---- */

static void raster_row(const dlist_t *d, int r)
{
	memset(rowidx, 0, d->w);
	for (int i = 0; i < d->n; i++) {
		const item_t *it = &d->it[i];
		if (r < it->y || r >= it->y + it->h) continue;
		int gr = r - it->y;
		if (it->kind == K_FILL) {
			int x0 = it->x < 0 ? 0 : it->x;
			int x1 = it->x + it->w > d->w ? d->w : it->x + it->w;
			for (int x = x0; x < x1; x++) rowidx[x] = (uint8_t)(it->color << 5);
			continue;
		}
		int fgbits = it->color << 2;
		for (int k = 0; k < it->len; k++) {
			int idx = d->ch[it->off + k];
			if (it->kind == K_BIG) {
				const unsigned char *g = &font_big[idx][gr * 3];
				int cx = it->x + k * BIG_W;
				for (int px = 0; px < BIG_W; px++) {
					int a = (g[px >> 2] >> (6 - 2 * (px & 3))) & 3;
					int xx = cx + px;
					if (a && xx >= 0 && xx < d->w)
						rowidx[xx] = (uint8_t)((rowidx[xx] & 0x60) | fgbits | a);
				}
			} else {
				unsigned char bits = font8x16[idx][gr];
				int cx = it->x + k * FONT_W;
				for (int px = 0; px < FONT_W; px++) {
					int xx = cx + px;
					if ((bits & (0x80 >> px)) && xx >= 0 && xx < d->w)
						rowidx[xx] = (uint8_t)((rowidx[xx] & 0x60) | fgbits | 3);
				}
			}
		}
	}
}

/* Downscaled row: dest row r of a list shrunk by inv/1024 (source px per
 * dest px). Each dest pixel keeps the most opaque glyph pixel it covers, so
 * thin strokes survive the reduction. */
static uint8_t accidx[MENU_W];
static void raster_scaled(const dlist_t *d, int r, int inv, int dw)
{
	int s0 = (r * inv) >> 10, s1 = ((r + 1) * inv) >> 10;
	if (s1 <= s0) s1 = s0 + 1;
	if (s1 > d->h) s1 = d->h;
	raster_row(d, s0);
	memcpy(accidx, rowidx, d->w);
	for (int s = s0 + 1; s < s1; s++) {
		raster_row(d, s);
		for (int x = 0; x < d->w; x++)
			if ((rowidx[x] & 3) > (accidx[x] & 3)) accidx[x] = rowidx[x];
	}
	for (int i = 0; i < dw; i++) {
		int x0 = (i * inv) >> 10, x1 = ((i + 1) * inv) >> 10;
		if (x1 <= x0) x1 = x0 + 1;
		if (x1 > d->w) x1 = d->w;
		uint8_t best = accidx[x0];
		for (int x = x0 + 1; x < x1; x++)
			if ((accidx[x] & 3) > (best & 3)) best = accidx[x];
		rowidx[i] = best;
	}
}

static void blit(const dlist_t *d, void *fb, int pitch, int fb_w, int fb_h, int x, int y,
                 int inv, int kern)
{
	if (inv < 1024) inv = 1024;
	int sw = d->w * 1024 / inv, sh = d->h * 1024 / inv;
	int sx = 0, sy = 0;
	if (sw <= fb_w && x + sw > fb_w) x = fb_w - sw;
	if (sh <= fb_h && y + sh > fb_h) y = fb_h - sh;
	if (x < 0) { sx = -x; x = 0; }   /* fb smaller than the overlay: clip */
	if (y < 0) { sy = -y; y = 0; }
	int cw = sw - sx, ch = sh - sy;
	if (cw > fb_w - x) cw = fb_w - x;
	if (ch > fb_h - y) ch = fb_h - y;
	if (cw <= 0 || ch <= 0) return;

	for (int r = 0; r < ch; r++) {
		if (inv == 1024) raster_row(d, sy + r);
		else raster_scaled(d, sy + r, inv, sw);
		for (int i = 0; i < cw; i++)
			rowbuf[i] = palette[rowidx[sx + i] & 0x7F];
		uint32_t *dst = (uint32_t *)fb + (uint32_t)(y + r) * pitch + x;
		if (kern) {
			memcpy(dst, rowbuf, cw * 4);
		} else {
			if (ksceKernelCopyToUser(dst, rowbuf, cw * 4) < 0)
				return;
		}
	}
}

void vz_blit_overlay(void *fb, int pitch, int fb_w, int fb_h, int ox, int oy, int inv)
{
	/* shared row buffers: skip this frame if another process is blitting */
	if (__sync_lock_test_and_set(&blit_busy, 1))
		return;
	init_palette();
	if (g_menu_open) {
		const dlist_t *d = &dls[DL_MENU][front[DL_MENU]];
		if (d->n)
			blit(d, fb, pitch, fb_w, fb_h, (fb_w - d->w) / 2, (fb_h - d->h) / 2, 1024, 0);
	} else if (vz_osd_visible()) {
		const dlist_t *d = &dls[DL_OSD][front[DL_OSD]];
		int m = 12 * 1024 / (inv < 1024 ? 1024 : inv);
		if (d->n)
			blit(d, fb, pitch, fb_w, fb_h, ox + m, oy + m, inv, 0);
	}
	__sync_lock_release(&blit_busy);
}

/* Menu drawn into our own (kernel) frame. */
void vz_blit_menu_kernel(void *fb, int pitch, int fb_w, int fb_h)
{
	while (__sync_lock_test_and_set(&blit_busy, 1)) ;
	init_palette();
	const dlist_t *d = &dls[DL_MENU][front[DL_MENU]];
	if (d->n)
		blit(d, fb, pitch, fb_w, fb_h, (fb_w - d->w) / 2, (fb_h - d->h) / 2, 1024, 1);
	__sync_lock_release(&blit_busy);
}

void vz_osd_show(uint32_t us)
{
	osd_until = ksceKernelGetSystemTimeLow() + us;
}

int vz_osd_visible(void)
{
	return (int32_t)(osd_until - ksceKernelGetSystemTimeLow()) > 0;
}
