/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host tests for VitaZoom's platform-independent logic: zoom levels,
 * gestures, settings, menu layout and overlay rasterising. The real sources
 * are compiled here; only the system calls are stubbed (see stubs.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <psp2kern/ctrl.h>
#include "../src/draw.c" /* included to inspect the display lists */

extern uint32_t test_now_us;
extern char test_root[256];
extern int test_snap_available;

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PID 0x4242

/* ---------------------------------------------------------------- helpers */

static void set_fb(int w, int h) { g_fb_w = w; g_fb_h = h; g_lvl_bad = 0; }

static uint64_t ts;
static void touch_at(uint32_t t, int n, int x0, int y0, int x1, int y1)
{
	int16_t xy[4] = {(int16_t)(x0 * 2), (int16_t)(y0 * 2), (int16_t)(x1 * 2), (int16_t)(y1 * 2)};
	test_now_us = t;
	vz_touch_sample(PID, ++ts, n, xy, 2);
}
static void up(uint32_t t) { touch_at(t, 0, 0, 0, 0, 0); }

static void reset_gesture(void)
{
	g_armed = 1; g_menu_open = 0; g_fg_pid = PID;
	vz_config_defaults();
	vz_gesture_reset();
	vz_set_view(0, 0, 0);
	test_now_us += 5000000; /* far from any previous gesture */
	up(test_now_us);
}

/* framebuffer point shown at screen point (sx, sy) */
static int fb_x(int sx) { return g_x0 + sx * vz_crop_w(g_level) / SCREEN_W; }
static int fb_y(int sy) { return g_y0 + sy * vz_crop_h(g_level) / SCREEN_H; }

/* two-finger pinch around (cx, cy), distance d0 -> d1 over 'ms' */
static uint32_t pinch(uint32_t t, int cx, int cy, int d0, int d1, int ms)
{
	for (int i = 0; i <= ms; i += 16) {
		int d = d0 + (d1 - d0) * i / ms;
		touch_at(t + i * 1000, 2, cx - d / 2, cy, cx + d / 2, cy);
	}
	return t + ms * 1000;
}

static void tap(uint32_t t, int x, int y)
{
	touch_at(t, 1, x, y, 0, 0);
	touch_at(t + 40000, 1, x, y, 0, 0);
	up(t + 80000);
}

/* ---------------------------------------------------------------- levels */

static void test_levels(void)
{
	static const int fbs[][2] = {{960, 544}, {720, 408}, {640, 368}, {480, 272}};
	for (int k = 0; k < VZ_LEVELS - 1; k++) {
		CHECK(vz_lvl_w[k] > vz_lvl_w[k + 1], "widths decrease at %d", k);
		CHECK(vz_lvl_h[k] > vz_lvl_h[k + 1], "heights decrease at %d", k);
		/* aspect ratio stays ~16:9 (within 2%) */
		int ar = vz_lvl_w[k] * 1000 / vz_lvl_h[k];
		CHECK(ar > 1730 && ar < 1800, "aspect of level %d = %d", k, ar);
	}
	CHECK(VZ_LEVELS <= 32, "g_lvl_bad holds every level");
	for (unsigned f = 0; f < sizeof(fbs) / sizeof(fbs[0]); f++) {
		set_fb(fbs[f][0], fbs[f][1]);
		int usable = 0, prev = 0;
		for (int l = 1; l < VZ_LEVELS; l++) {
			if (!vz_level_ok(l)) continue;
			usable++;
			CHECK(vz_lvl_w[l] < g_fb_w && vz_lvl_h[l] < g_fb_h, "level %d smaller than fb", l);
			/* viewport origin limit (x <= 640, y <= 364) can reach every edge */
			int vw = vz_lvl_w[l] * SCREEN_W / g_fb_w, vh = vz_lvl_h[l] * SCREEN_H / g_fb_h;
			CHECK(SCREEN_W - vw <= 640 && SCREEN_H - vh <= 364, "fb %d: level %d viewport %dx%d", g_fb_w, l, vw, vh);
			/* steps are fine: <= 6% between usable neighbours */
			if (prev) CHECK(vz_factor(l) * 100 / vz_factor(prev) <= 106, "fb %d: step %d->%d too big", g_fb_w, prev, l);
			prev = l;
		}
		CHECK(usable >= 10, "fb %dx%d has only %d levels", g_fb_w, g_fb_h, usable);
		CHECK(vz_factor(vz_max_level()) <= 3000 * g_fb_w / SCREEN_W + 100 || g_fb_w < SCREEN_W,
		      "fb %d: max zoom %d", g_fb_w, vz_factor(vz_max_level()));
		/* view is clamped inside the framebuffer */
		for (int l = 0; l < VZ_LEVELS; l++) {
			vz_set_view(l, 100000, 100000);
			CHECK(g_x0 + vz_crop_w(g_level) <= g_fb_w && g_y0 + vz_crop_h(g_level) <= g_fb_h,
			      "fb %d: view %d out of bounds", g_fb_w, l);
			CHECK(vz_level_ok(g_level), "fb %d: set_view kept an unusable level %d", g_fb_w, g_level);
			vz_set_view(l, -50, -50);
			CHECK(g_x0 == 0 && g_y0 == 0, "negative origin clamped");
		}
		/* step helpers stay in range */
		CHECK(vz_level_step(0, -1) == 0, "no step below 1x");
		CHECK(vz_level_step(vz_max_level(), 1) == vz_max_level(), "no step above max");
		char name[16];
		vz_level_name(0, name, sizeof(name));
		CHECK(!strcmp(name, "1.00x"), "1x name '%s'", name);
	}
	/* a refused level is skipped, and comes back on a new framebuffer size */
	set_fb(960, 544);
	g_lvl_bad = 1u << 10;
	CHECK(!vz_level_ok(10) && vz_level_step(9, 1) == 11, "bad level skipped");
	set_fb(960, 544);
	CHECK(vz_level_ok(10), "bad levels reset");
	/* documented sizes are present */
	int docs = 0;
	for (int l = 0; l < VZ_LEVELS; l++) docs += vz_lvl_documented(l);
	CHECK(docs == 3, "720/640/480 present (%d)", docs);
}

/* -------------------------------------------------------------- gestures */

static void test_pinch(void)
{
	set_fb(960, 544);
	reset_gesture();
	uint32_t t = test_now_us + 1000000;
	int ax = fb_x(480), ay = fb_y(272);
	t = pinch(t, 480, 272, 200, 400, 400); /* fingers twice as far apart */
	int f = vz_factor(g_level);
	CHECK(f > 1800 && f < 2200, "pinch x2 -> zoom %d (want ~2000)", f);
	CHECK(abs(fb_x(480) - ax) <= 3 && abs(fb_y(272) - ay) <= 3, "pinch anchor moved (%d,%d)->(%d,%d)",
	      ax, ay, fb_x(480), fb_y(272));
	/* hold still, then lift: no change */
	int l = g_level;
	t = pinch(t + 16000, 480, 272, 400, 400, 300);
	up(t + 16000);
	CHECK(g_level == l, "still fingers keep the level");

	/* lift jitter: a change in the last instant before lifting is undone */
	reset_gesture();
	t = test_now_us + 1000000;
	t = pinch(t, 480, 272, 200, 400, 400);
	t = pinch(t + 16000, 480, 272, 400, 400, 300);
	l = g_level;
	touch_at(t + 16000, 2, 480 - 170, 272, 480 + 170, 272); /* slid together */
	CHECK(g_level != l, "jitter changed the level (test setup)");
	up(t + 48000);
	CHECK(g_level == l, "lift jitter undone (%d -> %d)", l, g_level);

	/* a small close does not drop to 1x, a big one does */
	reset_gesture();
	t = test_now_us + 1000000;
	t = pinch(t, 480, 272, 300, 330, 200); /* ~1.1x */
	up(t + 16000);
	CHECK(g_level > 0, "zoomed a little");
	t = pinch(t + 600000, 480, 272, 300, 280, 200); /* close ~7% */
	up(t + 16000);
	CHECK(g_level > 0, "a small close keeps the zoom");
	t = pinch(t + 600000, 480, 272, 300, 150, 300);
	up(t + 16000);
	CHECK(g_level == 0, "a big close goes back to 1x (level %d)", g_level);

	/* strength: soft < normal < strong for the same finger movement */
	int res[3];
	for (int s = 0; s < 3; s++) {
		reset_gesture();
		g_cfg.pinch = s;
		t = pinch(test_now_us + 1000000, 480, 272, 200, 300, 300);
		up(t + 16000);
		res[s] = vz_factor(g_level);
	}
	CHECK(res[0] < res[1] && res[1] < res[2], "strength order %d %d %d", res[0], res[1], res[2]);
	CHECK(res[1] > 1400 && res[1] < 1600, "normal x1.5 -> %d", res[1]);

	/* small framebuffer (Adrenaline "Original"): the pinch is still smooth */
	set_fb(480, 272);
	reset_gesture();
	t = test_now_us + 1000000;
	int changes = 0, last = g_level;
	for (int i = 0; i <= 600; i += 16) {
		int d = 200 + 160 * i / 600;
		touch_at(t + i * 1000, 2, 480 - d / 2, 272, 480 + d / 2, 272);
		if (g_level != last) { changes++; last = g_level; }
	}
	CHECK(changes >= 8, "480x272: only %d level changes over a x1.8 pinch", changes);
	set_fb(960, 544);
}

static void test_long_press(void)
{
	set_fb(960, 544);
	reset_gesture();
	uint32_t t0 = test_now_us + 1000000;
	int sx = 300, sy = 200, ax = fb_x(sx), ay = fb_y(sy);
	int lv[4] = {0};
	for (int i = 0; i <= 3200; i += 16) {
		touch_at(t0 + i * 1000, 1, sx + (i / 16) % 3 - 1, sy, 0, 0); /* 1 px jitter */
		if (i == 400) lv[0] = g_level;
		if (i == 560) lv[1] = g_level;
		if (i == 1568) lv[2] = g_level;
		if (i == 3200) lv[3] = g_level;
	}
	CHECK(lv[0] == 0, "no zoom before 0.5 s");
	CHECK(lv[1] > 0, "first step at 0.5 s");
	int f1 = vz_factor(lv[1]);
	CHECK(f1 > 1400 && f1 < 1600, "first step +50%% -> %d", f1);
	CHECK(lv[2] > lv[1], "second step after 1 s more");
	CHECK(lv[3] == vz_max_level(), "keeps going up to the max (%d/%d)", lv[3], vz_max_level());
	CHECK(abs(fb_x(sx) - ax) <= 4 && abs(fb_y(sy) - ay) <= 4, "point under the finger moved (%d,%d)->(%d,%d)",
	      ax, ay, fb_x(sx), fb_y(sy));
	up(t0 + 3300000);
	CHECK(g_level == lv[3], "releasing keeps the zoom");

	/* slide after the hold: pans from where it is, no jump */
	reset_gesture();
	t0 = test_now_us + 1000000;
	for (int i = 0; i <= 600; i += 16) touch_at(t0 + i * 1000, 1, 480, 272, 0, 0);
	int x0 = g_x0, y0 = g_y0, l = g_level;
	touch_at(t0 + 616000, 1, 480 + 12, 272, 0, 0); /* just past the drag threshold */
	CHECK(g_level == l, "sliding does not zoom more");
	CHECK(abs(g_x0 - (x0 - 12 * vz_crop_w(l) / SCREEN_W)) <= 1 && g_y0 == y0,
	      "slide pans without a jump (%d -> %d)", x0, g_x0);
	for (int i = 0; i <= 2000; i += 16) touch_at(t0 + 632000 + i * 1000, 1, 480 + 12 + i / 100, 272, 0, 0);
	CHECK(g_level == l, "a moving finger never long-presses");
	up(t0 + 2700000);

	/* moving finger from the start: no zoom (drag only) */
	reset_gesture();
	t0 = test_now_us + 1000000;
	for (int i = 0; i <= 1500; i += 16) touch_at(t0 + i * 1000, 1, 200 + i / 20, 200, 0, 0);
	CHECK(g_level == 0, "dragging is not a long press");
	up(t0 + 1600000);

	/* "to the app" mode: one finger belongs to the app */
	reset_gesture();
	g_cfg.touch_mode = TOUCH_PASS;
	t0 = test_now_us + 1000000;
	for (int i = 0; i <= 1500; i += 16) touch_at(t0 + i * 1000, 1, 480, 272, 0, 0);
	CHECK(g_level == 0, "no long press in pass mode");
	up(t0 + 1600000);
}

static void test_taps(void)
{
	set_fb(960, 544);
	/* double tap resets */
	reset_gesture();
	vz_set_view(12, 100, 100);
	uint32_t t = test_now_us + 1000000;
	tap(t, 400, 300);
	CHECK(g_level == 12, "a single tap does nothing");
	tap(t + 200000, 405, 300);
	CHECK(g_level == 0, "double tap resets (level %d)", g_level);

	/* double tap at 1x stays at 1x */
	t += 2000000;
	tap(t, 400, 300);
	tap(t + 200000, 400, 300);
	CHECK(g_level == 0, "double tap at 1x stays 1x");

	/* taps too slow or too far apart */
	vz_set_view(12, 100, 100);
	t += 2000000;
	tap(t, 400, 300);
	tap(t + 600000, 400, 300);
	CHECK(g_level == 12, "slow taps are not a double tap");
	t += 2000000;
	tap(t, 100, 100);
	tap(t + 200000, 700, 400);
	CHECK(g_level == 12, "far taps are not a double tap");

	/* lifting and re-touching right after a drag is not a double tap */
	reset_gesture();
	vz_set_view(12, 100, 100);
	t = test_now_us + 1000000;
	for (int i = 0; i <= 300; i += 16) touch_at(t + i * 1000, 1, 300 + i / 4, 300, 0, 0);
	up(t + 316000);
	tap(t + 380000, 380, 300);
	tap(t + 560000, 380, 300);
	CHECK(g_level > 0, "taps right after a drag are ignored");

	/* a long press is not a tap */
	reset_gesture();
	vz_set_view(12, 100, 100);
	t = test_now_us + 1000000;
	tap(t, 400, 300);
	for (int i = 0; i <= 700; i += 16) touch_at(t + 150000 + i * 1000, 1, 400, 300, 0, 0);
	up(t + 900000);
	CHECK(g_level > 0, "tap + long press is not a double tap");

	/* disabled */
	reset_gesture();
	g_cfg.dtap_enable = 0;
	vz_set_view(12, 100, 100);
	t = test_now_us + 1000000;
	tap(t, 400, 300);
	tap(t + 200000, 400, 300);
	CHECK(g_level == 12, "double tap disabled");
}

static void test_arbiter(void)
{
	set_fb(960, 544);
	reset_gesture();
	uint32_t before = g_st.smp_rejected;
	int16_t xy[4] = {600, 400, 1400, 400};
	test_now_us += 1000000;
	vz_touch_sample(PID + 1, ++ts, 2, xy, 2); /* another process */
	CHECK(g_st.smp_rejected == before + 1, "other process rejected");
	vz_touch_sample(PID, ts, 2, xy, 2);
	vz_touch_sample(PID, ts, 0, xy, 2); /* same timestamp: stale */
	CHECK(g_st.smp_rejected == before + 2, "stale sample rejected");
	g_armed = 0;
	vz_touch_sample(PID, ++ts, 2, xy, 2);
	CHECK(g_st.smp_rejected == before + 3, "ignored while zoom is off");
	g_armed = 1;
	g_menu_open = 1;
	vz_touch_sample(PID, ++ts, 2, xy, 2);
	CHECK(g_st.smp_rejected == before + 4, "ignored while the menu is open");
	g_menu_open = 0;
}

/* -------------------------------------------------------------- settings */

static void test_config(void)
{
	set_fb(960, 544);
	vz_config_defaults();
	CHECK(vz_config_load_global() < 0, "no file yet");
	CHECK(g_cfg.menu_combo == (SCE_CTRL_DOWN | SCE_CTRL_SELECT), "default menu combo");

	g_cfg.menu_combo = SCE_CTRL_L1 | SCE_CTRL_START;
	g_cfg.toggle_combo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
	g_cfg.pinch = PINCH_STRONG;
	g_cfg.hold_step = 200;
	g_cfg.touch_mode = TOUCH_PASS;
	g_cfg.pan_stick = STICK_L_RIGHT;
	g_cfg.block_rear = 1;
	g_cfg.dtap_enable = 0;
	CHECK(vz_config_save_global() == 0, "save global");
	vz_config_t saved = g_cfg;
	vz_config_defaults();
	CHECK(vz_config_load_global() == 0, "load global");
	CHECK(!memcmp(&saved, &g_cfg, sizeof(saved)), "global round trip");

	/* per-app profile with the current view */
	g_armed = 1;
	vz_set_view(9, 120, 60);
	g_cfg.auto_arm = 1;
	g_cfg.pinch = PINCH_SOFT;
	CHECK(vz_config_save_app("ABCD00001") == 0, "save app");
	vz_config_load_app("OTHER0001");
	CHECK(!g_app_profile && g_cfg.pinch == PINCH_STRONG, "other app uses the default");
	vz_config_load_app("ABCD00001");
	CHECK(g_app_profile && g_cfg.pinch == PINCH_SOFT && g_cfg.auto_arm == 1, "app profile loaded");
	CHECK(g_cfg.view_w == vz_lvl_w[9] && g_cfg.view_x == 120 && g_cfg.view_y == 60, "saved view %d %d %d",
	      g_cfg.view_w, g_cfg.view_x, g_cfg.view_y);
	CHECK(g_cfg.menu_combo == saved.menu_combo, "shortcuts stay global");
	vz_config_delete_app("ABCD00001");
	CHECK(!g_app_profile && g_cfg.pinch == PINCH_STRONG, "profile removed");

	/* bad, old and unknown values are ignored; CRLF and comments are fine */
	char path[512];
	snprintf(path, sizeof(path), "%s/ur0/data/vitazoom/apps/BAD000001.txt", test_root);
	FILE *f = fopen(path, "w");
	fputs("# comment\r\npinch=7\r\nhold_step=175\r\ndtap_w=480\r\ndtap_level=2\r\ntouch_mode=9\r\n"
	      "view_w=5000\r\nnonsense\r\n=1\r\nblock_rear=0\r\n", f);
	fclose(f);
	vz_config_load_app("BAD000001");
	CHECK(g_cfg.pinch == PINCH_STRONG && g_cfg.hold_step == 200 && g_cfg.touch_mode == TOUCH_PASS &&
	      g_cfg.view_w == 0 && g_cfg.block_rear == 0, "bad values ignored");

	/* a huge file is read safely (only the first 1 KB) */
	f = fopen(path, "w");
	for (int i = 0; i < 4000; i++) fputs("pinch=0\n", f);
	fclose(f);
	vz_config_load_app("BAD000001");
	CHECK(g_cfg.pinch == PINCH_SOFT, "big file");
	vz_config_defaults();
	g_armed = 0;
	vz_set_view(0, 0, 0);
}

/* ------------------------------------------------------------ menu layout */

static int layout_ok(const dlist_t *d, const char *what)
{
	int ok = 1;
	for (int i = 0; i < d->n; i++) {
		const item_t *it = &d->it[i];
		if (it->x < 0 || it->y < 0 || it->x + it->w > d->w || it->y + it->h > d->h) {
			printf("  %s: item %d (%d,%d %dx%d) outside %dx%d\n", what, i, it->x, it->y, it->w, it->h, d->w, d->h);
			ok = 0;
		}
		if (it->kind == K_FILL) continue;
		/* texts on the same line must not overlap (the 8x16 font's last row
		 * is blank in every glyph, so compact rows 15 px apart are fine) */
		for (int j = i + 1; j < d->n; j++) {
			const item_t *o = &d->it[j];
			int ih = it->kind == K_SMALL ? it->h - 1 : it->h, oh = o->kind == K_SMALL ? o->h - 1 : o->h;
			if (o->kind == K_FILL || o->y >= it->y + ih || it->y >= o->y + oh) continue;
			if (o->x < it->x + it->w && it->x < o->x + o->w) {
				printf("  %s: texts %d and %d overlap\n", what, i, j);
				ok = 0;
			}
		}
	}
	CHECK(d->n > 0 && d->n < DL_ITEMS && d->nch < DL_CHARS, "%s: list size %d items %d chars", what, d->n, d->nch);
	return ok;
}

static void press(uint32_t b)
{
	test_now_us += 50000;
	vz_menu_input(b, b, test_now_us);
	test_now_us += 50000;
	vz_menu_input(0, 0, test_now_us);
}

static void check_all_rows(const char *what)
{
	char buf[96];
	for (int i = 0; i < 16; i++) {
		snprintf(buf, sizeof(buf), "%s row %d", what, i);
		CHECK(layout_ok(&dls[DL_MENU][front[DL_MENU]], buf), "%s", buf);
		press(SCE_CTRL_DOWN);
	}
}

static uint32_t fbmem[960 * 544];

static void test_menu(void)
{
	snprintf(g_app_tid, sizeof(g_app_tid), "PCSE00000");
	/* full menu on a 960x544 app */
	set_fb(960, 544);
	test_snap_available = 0;
	vz_config_defaults();
	vz_menu_open();
	CHECK(g_menu_open && g_block_buttons, "menu open blocks buttons");
	check_all_rows("full");
	/* every option value, both touch modes */
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < 16; i++) {
			for (int k = 0; k < 3; k++) {
				press(SCE_CTRL_RIGHT);
				CHECK(layout_ok(&dls[DL_MENU][front[DL_MENU]], "values"), "values row %d", i);
			}
			press(SCE_CTRL_DOWN);
		}
		g_cfg.touch_mode = TOUCH_PASS;
		vz_menu_render();
	}
	/* long combos and status lines */
	g_cfg.menu_combo = 0xF3F9u & VZ_BTN_MASK;
	g_cfg.toggle_combo = 0xFFF9u & VZ_BTN_MASK;
	vz_menu_render();
	CHECK(layout_ok(&dls[DL_MENU][front[DL_MENU]], "long combos"), "long combos");
	/* rasterise into a framebuffer of the exact size (ASan checks bounds) */
	vz_blit_overlay(fbmem, 960, 960, 544, 0, 0, 1024);
	vz_blit_menu_kernel(fbmem, 960, 960, 544);
	vz_config_defaults();
	vz_menu_close();
	CHECK(!g_menu_open, "menu closed");

	/* small screen without own frame: compact menu that fits */
	set_fb(480, 272);
	vz_menu_open();
	const dlist_t *d = &dls[DL_MENU][front[DL_MENU]];
	CHECK(d->w <= 480 && d->h <= 272, "compact menu %dx%d fits 480x272", d->w, d->h);
	check_all_rows("compact");
	static uint32_t small[512 * 272];
	vz_blit_overlay(small, 512, 480, 272, 0, 0, 1024);
	vz_menu_close();

	/* small screen with own frame: full menu */
	test_snap_available = 1;
	vz_menu_open();
	d = &dls[DL_MENU][front[DL_MENU]];
	CHECK(d->w == MENU_W && d->h == MENU_H, "own frame gets the full menu");
	vz_menu_close();
	test_snap_available = 0;

	/* the menu shortcut also closes it */
	set_fb(960, 544);
	vz_menu_open();
	test_now_us += 100000;
	vz_menu_input(g_cfg.menu_combo, g_cfg.menu_combo, test_now_us);
	CHECK(!g_menu_open, "shortcut closes the menu");
	vz_menu_open();
	press(SCE_CTRL_CIRCLE);
	CHECK(!g_menu_open, "circle closes the menu");
}

static void test_font(void)
{
	int used = 0;
	for (int c = 0; c < 95; c++) used |= font8x16[c][15];
	CHECK(!used, "small font: last glyph row is blank");
}

static void test_osd(void)
{
	static const char *msgs[] = {"Zoom 2.93x", "VitaZoom: zoom off", "VitaZoom: zoom on"};
	for (unsigned m = 0; m < 3; m++) {
		vz_osd(msgs[m]);
		CHECK(layout_ok(&dls[DL_OSD][front[DL_OSD]], msgs[m]), "osd %s", msgs[m]);
		CHECK(vz_osd_visible(), "osd visible");
	}
	/* shrunk OSD anywhere on small and large framebuffers stays in bounds */
	static uint32_t buf[512 * 272];
	for (int inv = 1024; inv <= 4096; inv += 512)
		for (int x = 0; x < 480; x += 97)
			for (int y = 0; y < 272; y += 61)
				vz_blit_overlay(buf, 512, 480, 272, x, y, inv);
	for (int inv = 1024; inv <= 4096; inv += 1024)
		vz_blit_overlay(fbmem, 960, 960, 544, 900, 520, inv);
	test_now_us += 1600000;
	CHECK(!vz_osd_visible(), "osd hides after 1.5 s");
}

int main(void)
{
	char tmpl[] = "/tmp/vitazoom-test-XXXXXX";
	if (!mkdtemp(tmpl)) return 2;
	snprintf(test_root, sizeof(test_root), "%s", tmpl);

	test_levels();
	test_pinch();
	test_long_press();
	test_taps();
	test_arbiter();
	test_config();
	test_menu();
	test_font();
	test_osd();

	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpl);
	if (system(cmd)) {}
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
