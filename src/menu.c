/* SPDX-License-Identifier: GPL-3.0-or-later
 * VitaZoom - pinch-to-zoom kernel plugin for PS Vita (taiHEN)
 * Copyright (C) 2026 VitaZoom contributors
 */
/*
 * menu.c - overlay menu driven by the worker thread.
 */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/ctrl.h>
#include "vitazoom.h"

#define FONT_W_SMALL 8

enum {
	IT_ARMED, IT_AUTO, IT_TOUCH, IT_STICK, IT_PINCH, IT_DTAP, IT_DTAP_LVL, IT_REAR,
	IT_MENU_COMBO, IT_TOGGLE_COMBO, IT_SAVE_APP, IT_SAVE_GLOBAL, IT_RESET_APP,
	IT_CLOSE, IT_COUNT
};

static const char *const item_label[IT_COUNT] = {
	"Zoom",
	"Turn on when this app opens",
	"One-finger touch",
	"Move with analog stick",
	"Pinch strength",
	"Double tap resets zoom",
	"Long-press zoom step",
	"Block rear touch",
	"Menu shortcut",
	"Quick zoom shortcut",
	"Save for this app",
	"Save as default (all apps)",
	"Use default for this app",
	"Close",
};

#define ROW_H   26
#define ITEMS_Y 70

static int sel;
static int capture;          /* 0 = no, else item being captured */
static uint32_t cap_btn, cap_t;
static int cap_released;
static uint32_t rep_btn, rep_t;
static char status[64];
static int status_fg;

static const struct { uint32_t m; const char *n; } btn_names[] = {
	{SCE_CTRL_SELECT, "SELECT"}, {SCE_CTRL_START, "START"},
	{SCE_CTRL_UP, "UP"}, {SCE_CTRL_DOWN, "DOWN"},
	{SCE_CTRL_LEFT, "LEFT"}, {SCE_CTRL_RIGHT, "RIGHT"},
	{SCE_CTRL_LTRIGGER, "L"}, {SCE_CTRL_RTRIGGER, "R"},
	{SCE_CTRL_L1, "L1"}, {SCE_CTRL_R1, "R1"},
	{SCE_CTRL_TRIANGLE, "TRIANGLE"}, {SCE_CTRL_CIRCLE, "CIRCLE"},
	{SCE_CTRL_CROSS, "X"}, {SCE_CTRL_SQUARE, "SQUARE"},
};

const char *vz_combo_str(uint32_t combo, char *buf, int len)
{
	buf[0] = 0;
	if (!combo) {
		snprintf(buf, len, "none");
		return buf;
	}
	int pos = 0;
	for (unsigned i = 0; i < sizeof(btn_names) / sizeof(btn_names[0]); i++) {
		if (!(combo & btn_names[i].m)) continue;
		pos += snprintf(buf + pos, len - pos, "%s%s", pos ? "+" : "", btn_names[i].n);
		if (pos >= len - 1) break;
	}
	return buf;
}

void vz_osd(const char *msg)
{
	vz_dl_begin(DL_OSD);
	int w = 28 + BIG_W * vz_text_len(msg);
	if (w > OSD_W) w = OSD_W;
	vz_fill(0, 0, w, OSD_H, BG_OSD);
	vz_fill(0, 0, 4, OSD_H, BG_SEL);
	vz_text_big(14, (OSD_H - BIG_H) / 2, msg, FG_WHITE);
	vz_dl_commit();
	vz_osd_show(1500000);
}

static void set_status(int fg, const char *msg)
{
	snprintf(status, sizeof(status), "%s", msg);
	status_fg = fg;
}

/* Shorten s (UTF-8) to at most max characters, ending with ".." when cut. */
static void fit(char *s, int max)
{
	if (max < 3) max = 3;
	if (vz_text_len(s) <= max) return;
	int n = 0;
	char *p = s;
	while (*p && n < max - 2) {
		p++;
		while ((*p & 0xC0) == 0x80) p++; /* continuation bytes */
		n++;
	}
	memcpy(p, "..", 3);
}

/* value right-aligned on a row whose left part ends at x 'used' */
static void right_text(int y, char *s, int fg, int used)
{
	fit(s, (MENU_W - 20 - used - BIG_W) / BIG_W);
	vz_text_big(MENU_W - 20 - BIG_W * vz_text_len(s), y, s, fg);
}

static int item_value(int i, char *val, int vlen, int zok)
{
	int vfg = FG_TEXT;
	val[0] = 0;
	switch (i) {
	case IT_ARMED:
		snprintf(val, vlen, g_armed ? "ON" : "off");
		if (g_armed) vfg = FG_OK;
		break;
	case IT_PINCH:
		snprintf(val, vlen, g_cfg.pinch == PINCH_SOFT ? "soft" :
		         g_cfg.pinch == PINCH_STRONG ? "strong" : "normal");
		break;
	case IT_AUTO: snprintf(val, vlen, g_cfg.auto_arm ? "yes" : "no"); break;
	case IT_TOUCH:
		snprintf(val, vlen, g_cfg.touch_mode == TOUCH_PASS ?
		         "to the app" : "zoom only");
		break;
	case IT_STICK:
		snprintf(val, vlen, g_cfg.pan_stick == STICK_RIGHT ? "right" :
		         g_cfg.pan_stick == STICK_L_RIGHT ? "L + right" : "no");
		break;
	case IT_DTAP:
		if (g_cfg.touch_mode == TOUCH_PASS) { snprintf(val, vlen, "-"); vfg = FG_DIM; }
		else snprintf(val, vlen, g_cfg.dtap_enable ? "yes" : "no");
		break;
	case IT_DTAP_LVL:
		if (g_cfg.touch_mode == TOUCH_PASS) { snprintf(val, vlen, "-"); vfg = FG_DIM; }
		else snprintf(val, vlen, "+%d%%", g_cfg.hold_step - 100);
		break;
	case IT_REAR: snprintf(val, vlen, g_cfg.block_rear ? "yes" : "no"); break;
	case IT_MENU_COMBO:
	case IT_TOGGLE_COMBO:
		if (capture == i) {
			snprintf(val, vlen, "hold...");
			vfg = FG_ACCENT;
		} else {
			vz_combo_str(i == IT_MENU_COMBO ? g_cfg.menu_combo : g_cfg.toggle_combo,
			             val, vlen);
		}
		break;
	}
	return vfg;
}

static void render_compact(void);

void vz_menu_render(void)
{
	if (!g_snap_on && (g_fb_w < MENU_W || g_fb_h < MENU_H)) {
		render_compact();
		return;
	}

	char tmp[96], val[48];

	vz_dl_begin(DL_MENU);
	vz_fill(0, 0, MENU_W, MENU_H, BG_PANEL);
	vz_fill(0, 0, MENU_W, 34, BG_HEAD);
	vz_fill(0, 34, MENU_W, 2, BG_SEL);
	int hx = vz_text_big(20, 5, "VitaZoom " VZ_VERSION, FG_ACCENT);
	vz_combo_str(g_cfg.menu_combo, val, sizeof(val));
	snprintf(tmp, sizeof(tmp), "%s opens/closes", val);
	if (vz_text_len(tmp) * BIG_W > MENU_W - 20 - hx - BIG_W)
		snprintf(tmp, sizeof(tmp), "%s", val); /* long shortcut: just the buttons */
	right_text(5, tmp, FG_DIM, hx);

	snprintf(tmp, sizeof(tmp), "App: %s  (%s)", g_app_tid[0] ? g_app_tid : "?",
	         g_app_profile ? "own profile" : "using default");
	vz_text_big(20, 40, tmp, g_app_profile ? FG_OK : FG_DIM);

	int zok = vz_max_level() > 0;
	int y = ITEMS_Y;
	for (int i = 0; i < IT_COUNT; i++, y += ROW_H) {
		if (i == IT_MENU_COMBO || i == IT_SAVE_APP) y += 4;
		if (i == sel)
			vz_fill(10, y - 1, MENU_W - 20, ROW_H, BG_SEL);
		int fg = i == sel ? FG_WHITE : FG_TEXT;
		if (i == IT_RESET_APP && !g_app_profile) fg = FG_DIM;
		int lx = vz_text_big(22, y, item_label[i], fg);

		int vfg = item_value(i, val, sizeof(val), zok);
		if (val[0]) right_text(y, val, i == sel && vfg == FG_TEXT ? FG_WHITE : vfg, lx);
	}

	/* live diagnostics (small font) */
	y += 4;
	if (g_vp_mode)
		snprintf(tmp, sizeof(tmp), "screen %dx%d pf%x  viewport r%x  levels %d",
		         g_fb_w, g_fb_h, g_st.fb_pf, (unsigned)g_vp_ret,
		         VZ_LEVELS - 1 - __builtin_popcount(g_lvl_bad));
	else
		snprintf(tmp, sizeof(tmp), "screen %dx%d pf%x  crop %u r%x  step %dpx",
		         g_fb_w, g_fb_h, g_st.fb_pf, g_st.zoom_calls, (unsigned)g_st.zoom_ret,
		         g_cfg.align_px);
	vz_text_small(22, y, tmp, zok ? FG_DIM : FG_WARN);
	snprintf(tmp, sizeof(tmp), "touch r%x %df  samples %u/%u  re-presents %u  stick %d,%d",
	         (unsigned)g_st.ctx_ret, g_st.ctx_n, g_st.smp_used, g_st.smp_rejected,
	         g_st.resubmits, g_st.stick_rx, g_st.stick_ry);
	vz_text_small(22, y + 16, tmp, FG_DIM);

	y += 38;
	if (capture)
		vz_text_big(20, y, "Hold the button combination for 1 second", FG_ACCENT);
	else if (g_zoom_fail)
		vz_text_big(20, y, "Warning: the display refused the crop", FG_WARN);
	else if (status[0])
		vz_text_big(20, y, status, status_fg);
	else if (!zok)
		vz_text_big(20, y, "This app uses a screen too small to zoom", FG_WARN);
	else if (sel == IT_TOGGLE_COMBO)
		vz_text_big(20, y, "TRIANGLE removes the quick shortcut", FG_DIM);
	else if (sel == IT_TOUCH)
		vz_text_big(20, y, "\"to the app\": zoom with 2 fingers only", FG_DIM);
	else if (sel == IT_PINCH)
		vz_text_big(20, y, "How much zoom each pinch makes", FG_DIM);
	else if (sel == IT_DTAP_LVL)
		vz_text_big(20, y, "Hold 1 finger: one more step per second", FG_DIM);
	else if (sel == IT_SAVE_APP)
		vz_text_big(20, y, "Also stores the current zoom as the start", FG_DIM);

	vz_fill(0, MENU_H - 30, MENU_W, 30, BG_HEAD);
	vz_text_big(20, MENU_H - 27, "X select   LEFT/RIGHT change   CIRCLE close", FG_DIM);
	vz_dl_commit();
}

/* small framebuffers (e.g. 480x272): same items, 8x16 font */
static void render_compact(void)
{
	char tmp[96], val[48];
	int w = g_fb_w - 8 < 472 ? g_fb_w - 8 : 472;
	int h = 16 + 2 + IT_COUNT * 15 + 4 + 16 + 16;
	if (h > g_fb_h) h = g_fb_h;
	int zok = vz_max_level() > 0;

	vz_dl_begin(DL_MENU);
	vz_dl_size(w, h);
	vz_fill(0, 0, w, h, BG_PANEL);
	vz_fill(0, 0, w, 17, BG_HEAD);
	snprintf(tmp, sizeof(tmp), "VitaZoom %s  %s", VZ_VERSION, g_app_tid);
	vz_text_small(6, 1, tmp, FG_ACCENT);
	int y = 19;
	for (int i = 0; i < IT_COUNT; i++, y += 15) {
		if (i == sel) vz_fill(2, y - 1, w - 4, 15, BG_SEL);
		int lx = vz_text_small(8, y - 1, item_label[i], i == sel ? FG_WHITE : FG_TEXT);
		int vfg = item_value(i, val, sizeof(val), zok);
		fit(val, (w - 8 - lx - FONT_W_SMALL) / FONT_W_SMALL);
		if (val[0])
			vz_text_small(w - 8 - FONT_W_SMALL * vz_text_len(val), y - 1, val, vfg);
	}
	y += 3;
	if (status[0]) vz_text_small(8, y, status, status_fg);
	else if (capture) vz_text_small(8, y, "Hold the combination for 1 s", FG_ACCENT);
	vz_text_small(8, h - 16, "X select  LEFT/RIGHT change  CIRCLE close", FG_DIM);
	vz_dl_commit();
}

void vz_menu_open(void)
{
	capture = 0;
	status[0] = 0;
	rep_btn = 0;
	vz_view_off();   /* the menu is never shown zoomed */
	vz_snap_begin(); /* own full-size frame on screens that can't hold the menu */
	vz_menu_render();
	g_block_buttons = 1;
	g_menu_open = 1;
	g_redraw = 1;
}

void vz_menu_close(void)
{
	capture = 0;
	g_menu_open = 0; /* g_block_buttons stays until all buttons are released */
	vz_snap_end();
	g_redraw = 1;
}

static void set_armed(int on)
{
	g_armed = on;
	vz_gesture_reset();
	if (!on) vz_set_view(0, 0, 0);
}

static void change(int item, int dir)
{
	switch (item) {
	case IT_ARMED: set_armed(!g_armed); break;
	case IT_PINCH: g_cfg.pinch = (g_cfg.pinch + 3 + dir) % 3; break;
	case IT_AUTO: g_cfg.auto_arm = !g_cfg.auto_arm; break;
	case IT_TOUCH: g_cfg.touch_mode = g_cfg.touch_mode == TOUCH_PASS ? TOUCH_BLOCK : TOUCH_PASS; break;
	case IT_STICK: g_cfg.pan_stick = (g_cfg.pan_stick + 3 + dir) % 3; break;
	case IT_DTAP: g_cfg.dtap_enable = !g_cfg.dtap_enable; break;
	case IT_REAR: g_cfg.block_rear = !g_cfg.block_rear; break;
	case IT_DTAP_LVL: { /* +25%, +50%, +100% per step */
		static const int steps[3] = {125, 150, 200};
		int i = g_cfg.hold_step == 125 ? 0 : g_cfg.hold_step == 200 ? 2 : 1;
		g_cfg.hold_step = steps[(i + 3 + dir) % 3];
		break;
	}
	}
}

static void activate(int item, uint32_t now)
{
	switch (item) {
	case IT_MENU_COMBO:
	case IT_TOGGLE_COMBO:
		capture = item;
		cap_released = 0;
		cap_btn = 0;
		cap_t = now;
		break;
	case IT_SAVE_APP:
		if (!g_app_tid[0]) { set_status(FG_WARN, "Unknown app"); break; }
		if (vz_config_save_app(g_app_tid) >= 0) set_status(FG_OK, "App profile saved.");
		else set_status(FG_WARN, "Save failed!");
		break;
	case IT_SAVE_GLOBAL:
		if (vz_config_save_global() >= 0) set_status(FG_OK, "Default saved.");
		else set_status(FG_WARN, "Save failed!");
		break;
	case IT_RESET_APP:
		if (!g_app_profile) { set_status(FG_DIM, "This app already uses the default."); break; }
		vz_config_delete_app(g_app_tid);
		set_status(FG_OK, "Profile removed: using the default.");
		break;
	case IT_CLOSE:
		vz_menu_close();
		break;
	default:
		change(item, 1);
	}
}

static void capture_input(uint32_t held, uint32_t now)
{
	held &= ~SCE_CTRL_PSBUTTON;
	if (!cap_released) {
		if (!held) cap_released = 1;
		return;
	}
	if (held != cap_btn) {
		cap_btn = held;
		cap_t = now;
		return;
	}
	if (!held || now - cap_t < 1000000) return;
	int nbits = 0;
	for (uint32_t v = held; v; v &= v - 1) nbits++;
	if (nbits < 2) {
		set_status(FG_WARN, "Use at least 2 buttons.");
		cap_released = 0;
		return;
	}
	if (capture == IT_MENU_COMBO) g_cfg.menu_combo = held;
	else g_cfg.toggle_combo = held;
	set_status(FG_OK, "Shortcut set. Save to keep it.");
	capture = 0;
}

/* held = buttons down now, pressed = newly pressed this tick */
void vz_menu_input(uint32_t held, uint32_t pressed, uint32_t now)
{
	if (capture) {
		capture_input(held, now);
		vz_menu_render();
		return;
	}

	/* the same shortcut that opens the menu also closes it */
	if ((held & g_cfg.menu_combo) == g_cfg.menu_combo && (pressed & g_cfg.menu_combo)) {
		vz_menu_close();
		return;
	}

	/* auto-repeat for the d-pad */
	uint32_t dpad = held & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
	uint32_t act = pressed;
	if (dpad && dpad == rep_btn) {
		if (now - rep_t > 350000) {
			act |= dpad;
			rep_t = now - 250000; /* repeat every 100 ms */
		}
	} else {
		rep_btn = dpad;
		rep_t = now;
	}

	if (act & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT))
		status[0] = 0;
	if (act & SCE_CTRL_UP) sel = (sel + IT_COUNT - 1) % IT_COUNT;
	if (act & SCE_CTRL_DOWN) sel = (sel + 1) % IT_COUNT;
	if (act & SCE_CTRL_LEFT) change(sel, -1);
	if (act & SCE_CTRL_RIGHT) change(sel, 1);
	if (pressed & SCE_CTRL_CROSS) activate(sel, now);
	if ((pressed & SCE_CTRL_TRIANGLE) && sel == IT_TOGGLE_COMBO) g_cfg.toggle_combo = 0;
	if (pressed & SCE_CTRL_CIRCLE) vz_menu_close();

	if (g_menu_open) vz_menu_render();
}
