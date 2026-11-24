/* SPDX-License-Identifier: GPL-3.0-or-later
 * VitaZoom - pinch-to-zoom kernel plugin for PS Vita (taiHEN)
 * Copyright (C) 2026 VitaZoom contributors
 */
/*
 * gesture.c - pinch, long press, drag and double tap on the front touch panel.
 * All coordinates are screen pixels (960x544); math is integer-only.
 *
 * Several sources deliver touch samples (our read in the app's display
 * context, the app's own reads, kernel readers). Touch is routed only to the
 * focused process, so samples from other processes (e.g. SceShell in the
 * background, always "0 fingers") would look like releases and fake taps.
 * vz_touch_sample() therefore accepts only samples from the foreground
 * process and only when newer than the last accepted one.
 */
#include <psp2kern/kernel/threadmgr.h>
#include "vitazoom.h"

#define TAP_MAX_US      250000
#define TAP_MAX_MOVE    16
#define DTAP_MAX_US     350000
#define DTAP_MAX_DIST   60
#define DRAG_THRESHOLD  10     /* finger jitter while holding is not a drag */
#define HOLD_FIRST_US   500000 /* long press: first zoom step */
#define HOLD_REPEAT_US  1000000 /* then one more step per second while held */
#define GESTURE_QUIET_US 450000 /* no taps right after a drag or pinch */
#define PINCH_SETTLE_US  150000 /* level change this close to lifting = jitter */
#define PINCH_STEP_HYST  3      /* % hysteresis between (fine) levels */
#define PINCH_OFF_HYST   14     /* % extra shrink needed to drop back to 1x */

volatile int g_touch_latch;

static volatile int lock;
static uint64_t last_ts;
static SceUID last_pid = -1;

static int prev_n;
/* one finger */
static int s_sx, s_sy, s_x0, s_y0, s_moved, s_notap, s_held;
static uint32_t s_t, s_hold_next;
/* last tap for double-tap detection */
static uint32_t tap_t;
static int tap_x, tap_y, tap_valid;
/* two fingers */
static int p_d0, p_f0, p_ax, p_ay, p_lvl;
/* recent pinch states, to undo the zoom-out of fingers sliding together
 * while they are lifted */
#define PINCH_HIST 16
static struct { uint32_t t; int l, x, y; } p_hist[PINCH_HIST];
static int p_hist_n, p_hist_i;
static uint32_t gesture_end_t;   /* last drag / pinch end */

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
	prev_n = 0;
	tap_valid = 0;
	last_ts = 0;
}

/* screen point -> framebuffer point for the current view */
static int src_x(int sx) { return g_x0 + sx * vz_crop_w(g_level) / SCREEN_W; }
static int src_y(int sy) { return g_y0 + sy * vz_crop_h(g_level) / SCREEN_H; }

static void on_tap(int sx, int sy, uint32_t now)
{
	if (!g_cfg.dtap_enable) return;
	/* lifting and re-touching to keep dragging is not a double tap */
	if (now - gesture_end_t < GESTURE_QUIET_US) {
		tap_valid = 0;
		return;
	}
	if (tap_valid && (now - tap_t) < DTAP_MAX_US &&
	    iabs(sx - tap_x) < DTAP_MAX_DIST && iabs(sy - tap_y) < DTAP_MAX_DIST) {
		tap_valid = 0;
		vz_set_view(0, 0, 0); /* double tap: back to the normal screen */
		return;
	}
	tap_valid = 1;
	tap_t = now;
	tap_x = sx;
	tap_y = sy;
}

/* long press: one more zoom step, keeping the point under the finger */
static void hold_step(int sx, int sy)
{
	int cur = g_level;
	int l = vz_level_for_width(vz_crop_w(cur) * 100 / g_cfg.hold_step);
	if (l <= cur) l = vz_level_step(cur, 1); /* always progress while possible */
	if (l <= cur) return;                    /* already at the maximum */
	int px = src_x(sx), py = src_y(sy);
	vz_set_view(l, px - sx * vz_crop_w(l) / SCREEN_W, py - sy * vz_crop_h(l) / SCREEN_H);
}

static void update(const vz_touch_t *t, uint32_t now)
{
	int n = t->n > 2 ? 2 : t->n;

	/* multi-finger latch: set by 2 fingers, cleared when all are lifted */
	if (n >= 2) g_touch_latch = 1;
	else if (n == 0) g_touch_latch = 0;

	/* "corrected touch" mode: one finger belongs to the app, so only
	 * two-finger gestures (pinch + pan) drive the zoom */
	if (g_cfg.touch_mode == TOUCH_PASS && n == 1) {
		prev_n = 0;
		return;
	}

	/* pinch ended: fingers slide together while they are lifted, so a
	 * zoom-out in the last instants is jitter: go back to the zoom of
	 * PINCH_SETTLE_US ago (a zoom-in, which is deliberate, is kept) */
	if (prev_n == 2 && n < 2) {
		int found = -1;
		for (int k = 0; k < p_hist_n; k++) {
			int i = (p_hist_i - 1 - k + PINCH_HIST) % PINCH_HIST;
			found = i;
			if (now - p_hist[i].t >= PINCH_SETTLE_US) break;
		}
		if (found >= 0 && p_hist[found].l > g_level)
			vz_set_view(p_hist[found].l, p_hist[found].x, p_hist[found].y);
		gesture_end_t = now;
		tap_valid = 0;
	}

	if (n == 0) {
		if (prev_n == 1 && s_moved) {
			gesture_end_t = now;
			tap_valid = 0;
		}
		if (prev_n == 1 && !s_moved && !s_notap && !s_held && (now - s_t) < TAP_MAX_US &&
		    g_cfg.touch_mode == TOUCH_BLOCK)
			on_tap(s_sx, s_sy, now);
		prev_n = 0;
		return;
	}

	if (n == 1) {
		int sx = t->x[0], sy = t->y[0];
		if (prev_n != 1) {
			s_notap = (prev_n == 2);
			s_sx = sx; s_sy = sy;
			s_x0 = g_x0; s_y0 = g_y0;
			s_moved = 0;
			s_held = 0;
			s_t = now;
			s_hold_next = now + HOLD_FIRST_US;
		} else {
			int dx = sx - s_sx, dy = sy - s_sy;
			if (iabs(dx) > DRAG_THRESHOLD || iabs(dy) > DRAG_THRESHOLD)
				s_moved = 1;
			if (s_moved && iabs(dx) + iabs(dy) > TAP_MAX_MOVE)
				tap_valid = 0;
			/* still finger: zoom in step by step while it is held */
			if (!s_moved && !s_notap && g_cfg.touch_mode == TOUCH_BLOCK &&
			    (int32_t)(now - s_hold_next) >= 0) {
				hold_step(sx, sy);
				s_held = 1;
				s_hold_next = now + HOLD_REPEAT_US;
				/* a slide after the hold moves the new view, without a jump */
				s_sx = sx; s_sy = sy;
				s_x0 = g_x0; s_y0 = g_y0;
			}
			int l = g_level;
			if (s_moved && l > 0)
				vz_set_view(l, s_x0 - dx * vz_crop_w(l) / SCREEN_W,
				               s_y0 - dy * vz_crop_h(l) / SCREEN_H);
		}
		prev_n = 1;
		return;
	}

	/* two fingers: pinch zoom anchored on the midpoint, plus pan */
	int mx = (t->x[0] + t->x[1]) / 2;
	int my = (t->y[0] + t->y[1]) / 2;
	int dx = t->x[0] - t->x[1], dy = t->y[0] - t->y[1];
	int dist = isqrt(dx * dx + dy * dy);
	tap_valid = 0;

	if (prev_n != 2) {
		p_d0 = dist < 20 ? 20 : dist;
		p_f0 = vz_factor(g_level);
		p_ax = src_x(mx);
		p_ay = src_y(my);
		p_lvl = g_level;
		p_hist_n = p_hist_i = 0;
	} else {
		/* how much zoom a pinch makes: the finger distance change is scaled
		 * (soft = half, strong = double) */
		static const int strength[3] = {50, 100, 200};
		int ratio = dist * 1000 / p_d0 - 1000;
		ratio = 1000 + ratio * strength[g_cfg.pinch] / 100;
		if (ratio < 100) ratio = 100;
		int target = p_f0 * ratio / 1000;
		/* move between usable levels at geometric midpoints, with hysteresis */
		for (;;) {
			int up = vz_level_step(p_lvl, 1);
			if (up == p_lvl) break;
			int mid = isqrt(vz_factor(p_lvl) * vz_factor(up));
			if (target <= mid * (100 + PINCH_STEP_HYST) / 100) break;
			p_lvl = up;
		}
		for (;;) {
			int dn = vz_level_step(p_lvl, -1);
			if (dn == p_lvl) break;
			int mid = isqrt(vz_factor(p_lvl) * vz_factor(dn));
			int h = dn == 0 ? PINCH_STEP_HYST + PINCH_OFF_HYST : PINCH_STEP_HYST;
			if (target >= mid * (100 - h) / 100) break;
			p_lvl = dn;
		}
		vz_set_view(p_lvl, p_ax - mx * vz_crop_w(p_lvl) / SCREEN_W,
		                   p_ay - my * vz_crop_h(p_lvl) / SCREEN_H);
	}
	p_hist[p_hist_i].t = now;
	p_hist[p_hist_i].l = g_level;
	p_hist[p_hist_i].x = g_x0;
	p_hist[p_hist_i].y = g_y0;
	p_hist_i = (p_hist_i + 1) % PINCH_HIST;
	if (p_hist_n < PINCH_HIST) p_hist_n++;
	prev_n = 2;
}

void vz_touch_sample(SceUID pid, uint64_t ts, int n, const int16_t *xy, int stride)
{
	if (!g_armed || g_menu_open || pid != g_fg_pid) {
		g_st.smp_rejected++;
		return;
	}
	if (__sync_lock_test_and_set(&lock, 1))
		return; /* another thread is processing a sample right now */

	if (pid != last_pid) {
		/* new foreground app: restart gesture tracking */
		last_pid = pid;
		last_ts = 0;
		prev_n = 0;
		tap_valid = 0;
	}
	if (ts != 0 && ts <= last_ts) {
		g_st.smp_rejected++;
	} else {
		if (ts) last_ts = ts;
		vz_touch_t t;
		t.n = n > 2 ? 2 : (n < 0 ? 0 : n);
		for (int i = 0; i < t.n; i++) {
			t.x[i] = xy[i * stride] / 2;     /* panel is 1920x1088 */
			t.y[i] = xy[i * stride + 1] / 2;
		}
		g_st.smp_used++;
		g_st.last_n = t.n;
		update(&t, ksceKernelGetSystemTimeLow());
	}
	__sync_lock_release(&lock);
}
