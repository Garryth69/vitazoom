/* SPDX-License-Identifier: GPL-3.0-or-later
 * VitaZoom - pinch-to-zoom kernel plugin for PS Vita (taiHEN)
 * Copyright (C) 2026 VitaZoom contributors
 */
/*
 * gesture.c - pinch, drag and double tap on the touch panels.
 * All coordinates are screen pixels (960x544); math is integer-only.
 *
 * Each panel (front, and rear when enabled) has its own gesture state, so a
 * finger on one panel never turns a gesture on the other into something
 * else.
 *
 * Several sources deliver touch samples (our read in the app's display
 * context, the app's own reads, kernel readers). Touch is routed only to the
 * focused process, so samples from other processes (e.g. SceShell in the
 * background, always "0 fingers") would look like releases and fake taps.
 * vz_touch_sample() therefore accepts only samples from the foreground
 * process and only when newer than the last accepted one of that panel.
 */
#include <psp2kern/kernel/threadmgr.h>
#include "vitazoom.h"

#define TAP_MAX_US      250000
#define TAP_MAX_MOVE    16
#define DTAP_MAX_US     350000
#define DTAP_MAX_DIST   60
#define DRAG_THRESHOLD  6
#define GESTURE_QUIET_US 450000 /* no taps right after a drag or pinch */
#define PINCH_SETTLE_US  150000 /* zoom-out this close to lifting = jitter */
#define PINCH_STEP_HYST  3      /* % hysteresis between (fine) levels */
#define PINCH_OFF_HYST   14     /* % extra shrink needed to drop back to 1x */
#define PINCH_HIST       16

/* rear panel active area (front is 1920x1088) */
#define REAR_Y_MIN 108
#define REAR_Y_MAX 889

volatile int g_touch_latch;

typedef struct {
	uint64_t last_ts;
	int prev_n;
	/* one finger */
	int s_sx, s_sy, s_x0, s_y0, s_moved, s_notap;
	uint32_t s_t;
	/* last tap, for double-tap detection */
	uint32_t tap_t;
	int tap_x, tap_y, tap_valid;
	/* two fingers */
	int p_d0, p_f0, p_ax, p_ay, p_lvl;
	/* recent pinch states, to undo the zoom-out of fingers sliding together
	 * while they are lifted */
	struct { uint32_t t; int l, x, y; } hist[PINCH_HIST];
	int hist_n, hist_i;
	uint32_t end_t; /* last drag / pinch end */
} panel_t;

static panel_t panels[2];
static volatile int lock;
static SceUID last_pid = -1;

static int iabs(int v) { return v < 0 ? -v : v; }

static int isqrt(int v)
{
	if (v <= 0) return 0;
	int r = 0, bit = 1 << 30;
	while (bit > v) bit >>= 2;
	while (bit) {
		if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
		else r >>= 1;
		bit >>= 2;
	}
	return r;
}

void vz_gesture_reset(void)
{
	for (int i = 0; i < 2; i++) {
		panels[i].prev_n = 0;
		panels[i].tap_valid = 0;
		panels[i].last_ts = 0;
	}
	g_touch_latch = 0;
}

/* screen point -> framebuffer point for the current view */
static int src_x(int sx) { return g_x0 + sx * vz_crop_w(g_level) / SCREEN_W; }
static int src_y(int sy) { return g_y0 + sy * vz_crop_h(g_level) / SCREEN_H; }

/* double tap: zoom in on the tapped point at 1x, back to 1x when zoomed */
static void on_tap(panel_t *p, int sx, int sy, uint32_t now)
{
	if (!g_cfg.dtap_enable) return;
	/* lifting and re-touching to keep dragging is not a double tap */
	if (now - p->end_t < GESTURE_QUIET_US) {
		p->tap_valid = 0;
		return;
	}
	if (p->tap_valid && (now - p->tap_t) < DTAP_MAX_US &&
	    iabs(sx - p->tap_x) < DTAP_MAX_DIST && iabs(sy - p->tap_y) < DTAP_MAX_DIST) {
		p->tap_valid = 0;
		if (g_level > 0) {
			vz_set_view(0, 0, 0);
		} else {
			/* the same factor on every framebuffer size */
			int l = vz_level_for_width(g_fb_w * 100 / g_cfg.dtap_zoom);
			if (l == 0) return;
			int px = src_x(sx), py = src_y(sy);
			vz_set_view(l, px - vz_crop_w(l) / 2, py - vz_crop_h(l) / 2);
		}
		return;
	}
	p->tap_valid = 1;
	p->tap_t = now;
	p->tap_x = sx;
	p->tap_y = sy;
}

static void pinch(panel_t *p, const vz_touch_t *t, uint32_t now)
{
	int mx = (t->x[0] + t->x[1]) / 2;
	int my = (t->y[0] + t->y[1]) / 2;
	int dx = t->x[0] - t->x[1], dy = t->y[0] - t->y[1];
	int dist = isqrt(dx * dx + dy * dy);
	p->tap_valid = 0;

	if (p->prev_n != 2) {
		/* start: the point between the fingers stays under them */
		p->p_d0 = dist < 20 ? 20 : dist;
		p->p_f0 = vz_factor(g_level);
		p->p_ax = src_x(mx);
		p->p_ay = src_y(my);
		p->p_lvl = g_level;
		p->hist_n = p->hist_i = 0;
	} else {
		/* how much zoom a pinch makes: the finger distance change is scaled
		 * (soft = half, strong = double) */
		static const int strength[3] = {50, 100, 200};
		int ratio = dist * 1000 / p->p_d0 - 1000;
		ratio = 1000 + ratio * strength[g_cfg.pinch] / 100;
		if (ratio < 100) ratio = 100;
		int target = p->p_f0 * ratio / 1000;
		/* move between usable levels at geometric midpoints, with hysteresis */
		for (;;) {
			int up = vz_level_step(p->p_lvl, 1);
			if (up == p->p_lvl) break;
			int mid = isqrt(vz_factor(p->p_lvl) * vz_factor(up));
			if (target <= mid * (100 + PINCH_STEP_HYST) / 100) break;
			p->p_lvl = up;
		}
		for (;;) {
			int dn = vz_level_step(p->p_lvl, -1);
			if (dn == p->p_lvl) break;
			int mid = isqrt(vz_factor(p->p_lvl) * vz_factor(dn));
			int h = dn == 0 ? PINCH_STEP_HYST + PINCH_OFF_HYST : PINCH_STEP_HYST;
			if (target >= mid * (100 - h) / 100) break;
			p->p_lvl = dn;
		}
		vz_set_view(p->p_lvl, p->p_ax - mx * vz_crop_w(p->p_lvl) / SCREEN_W,
		                      p->p_ay - my * vz_crop_h(p->p_lvl) / SCREEN_H);
	}
	p->hist[p->hist_i].t = now;
	p->hist[p->hist_i].l = g_level;
	p->hist[p->hist_i].x = g_x0;
	p->hist[p->hist_i].y = g_y0;
	p->hist_i = (p->hist_i + 1) % PINCH_HIST;
	if (p->hist_n < PINCH_HIST) p->hist_n++;
}

/* pinch ended: fingers slide together while they are lifted, so a zoom-out
 * in the last instants is jitter: go back to the zoom of PINCH_SETTLE_US ago
 * (a zoom-in, which is deliberate, is kept) */
static void pinch_end(panel_t *p, uint32_t now)
{
	int found = -1;
	for (int k = 0; k < p->hist_n; k++) {
		int i = (p->hist_i - 1 - k + PINCH_HIST) % PINCH_HIST;
		found = i;
		if (now - p->hist[i].t >= PINCH_SETTLE_US) break;
	}
	if (found >= 0 && p->hist[found].l > g_level)
		vz_set_view(p->hist[found].l, p->hist[found].x, p->hist[found].y);
	p->end_t = now;
	p->tap_valid = 0;
}

static void update(panel_t *p, int port, const vz_touch_t *t, uint32_t now)
{
	int n = t->n > 2 ? 2 : t->n;

	/* front multi-finger latch (hooks.c keeps it from the app) */
	if (port == 0) {
		if (n >= 2) g_touch_latch = 1;
		else if (n == 0) g_touch_latch = 0;
	}

	/* "to the app" mode: one front finger belongs to the app, so only
	 * two-finger gestures drive the zoom there */
	if (port == 0 && g_cfg.touch_mode == TOUCH_PASS && n == 1) {
		if (p->prev_n == 2) pinch_end(p, now);
		p->prev_n = 0;
		return;
	}

	if (p->prev_n == 2 && n < 2)
		pinch_end(p, now);

	if (n == 0) {
		if (p->prev_n == 1 && p->s_moved) {
			p->end_t = now;
			p->tap_valid = 0;
		}
		if (p->prev_n == 1 && !p->s_moved && !p->s_notap && (now - p->s_t) < TAP_MAX_US)
			on_tap(p, p->s_sx, p->s_sy, now);
		p->prev_n = 0;
		return;
	}

	if (n == 1) {
		int sx = t->x[0], sy = t->y[0];
		if (p->prev_n != 1) {
			/* new finger (or one left over from a pinch: not a tap) */
			p->s_notap = (p->prev_n == 2);
			p->s_sx = sx; p->s_sy = sy;
			p->s_x0 = g_x0; p->s_y0 = g_y0;
			p->s_moved = 0;
			p->s_t = now;
		} else {
			int dx = sx - p->s_sx, dy = sy - p->s_sy;
			if (iabs(dx) > DRAG_THRESHOLD || iabs(dy) > DRAG_THRESHOLD)
				p->s_moved = 1;
			if (p->s_moved && iabs(dx) + iabs(dy) > TAP_MAX_MOVE)
				p->tap_valid = 0;
			/* drag: the image follows the finger */
			int l = g_level;
			if (p->s_moved && l > 0)
				vz_set_view(l, p->s_x0 - dx * vz_crop_w(l) / SCREEN_W,
				               p->s_y0 - dy * vz_crop_h(l) / SCREEN_H);
		}
		p->prev_n = 1;
		return;
	}

	pinch(p, t, now);
	p->prev_n = 2;
}

void vz_touch_sample(SceUID pid, int port, uint64_t ts, int n, const int16_t *xy, int stride)
{
	if (port != 0 && (port != 1 || g_cfg.rear_mode != REAR_ZOOM)) return;
	if (!g_armed || g_menu_open || pid != g_fg_pid) {
		g_st.smp_rejected++;
		return;
	}
	if (__sync_lock_test_and_set(&lock, 1))
		return; /* another thread is processing a sample right now */

	if (pid != last_pid) {
		/* new foreground app: restart gesture tracking */
		last_pid = pid;
		vz_gesture_reset();
	}
	panel_t *p = &panels[port];
	if (ts != 0 && ts <= p->last_ts) {
		g_st.smp_rejected++;
	} else {
		if (ts) p->last_ts = ts;
		vz_touch_t t;
		t.n = n > 2 ? 2 : (n < 0 ? 0 : n);
		for (int i = 0; i < t.n; i++) {
			int x = xy[i * stride], y = xy[i * stride + 1];
			if (port == 1) /* rear: same orientation, smaller active area */
				y = (y - REAR_Y_MIN) * PANEL_H / (REAR_Y_MAX - REAR_Y_MIN);
			if (x < 0) x = 0;
			if (x > PANEL_W - 1) x = PANEL_W - 1;
			if (y < 0) y = 0;
			if (y > PANEL_H - 1) y = PANEL_H - 1;
			t.x[i] = x / 2; /* panel units -> screen pixels */
			t.y[i] = y / 2;
		}
		g_st.smp_used++;
		g_st.last_n = t.n;
		update(p, port, &t, ksceKernelGetSystemTimeLow());
	}
	__sync_lock_release(&lock);
}
