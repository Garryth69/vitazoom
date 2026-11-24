/* SPDX-License-Identifier: GPL-3.0-or-later
 * VitaZoom - pinch-to-zoom kernel plugin for PS Vita (taiHEN)
 * Copyright (C) 2026 VitaZoom contributors
 */
/*
 * main.c - module entry, worker thread (buttons, menu, per-app profiles), log.
 */
#include <stdarg.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <psp2kern/ctrl.h>
#include <taihen.h>
#include "vitazoom.h"

int ksceAppMgrIsExclusiveProcessRunning(void);

volatile int g_armed = 0;
volatile int g_menu_open = 0;
volatile int g_block_buttons = 0;
volatile vz_stats_t g_st;
SceUID g_worker_thid = -1;

static volatile int running = 0;

/* ---------- log: ux0:data/vitazoom/log.txt (worker thread only) ---------- */

#define LOG_DIR  "ux0:data/vitazoom"
#define LOG_PATH LOG_DIR "/log.txt"
#define LOG_OLD  LOG_DIR "/log.1.txt" /* the previous boot, kept for crash reports */
static int log_lines;

void vz_log(const char *fmt, ...)
{
	if (log_lines > 1500) return; /* keep the file small */
	char buf[320];
	va_list ap;
	va_start(ap, fmt);
	int len = snprintf(buf, sizeof(buf), "[%7u] ", ksceKernelGetSystemTimeLow() / 1000);
	len += vsnprintf(buf + len, sizeof(buf) - len - 1, fmt, ap);
	va_end(ap);
	if (len > (int)sizeof(buf) - 2) len = sizeof(buf) - 2;
	buf[len++] = '\n';
	SceUID fd = ksceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0) return;
	ksceIoWrite(fd, buf, len);
	ksceIoClose(fd);
	log_lines++;
}

static void log_begin(void)
{
	ksceIoMkdir(LOG_DIR, 0777);
	ksceIoRemove(LOG_OLD);
	ksceIoRename(LOG_PATH, LOG_OLD);
	SceUID fd = ksceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd >= 0) ksceIoClose(fd);
	vz_log("VitaZoom " VZ_VERSION " started. hooks: ctrl u=%d/12 k=%d/4 touch u=%d/4 k=%d/4 vblank=%d/4",
	       g_st.ctrl_u_hooks, g_st.ctrl_k_hooks, g_st.touch_u_hooks, g_st.touch_k_hooks,
	       g_st.vblank_hooks);
}

static void log_state(const char *why)
{
	vz_log("%s: app=%s pid=0x%x armed=%d menu=%d lvl=%d x0=%d y0=%d | fb %dx%d pitch=%d pf=0x%x idx=%d "
	       "calls=%u | zoom calls=%u ret=0x%08x fail=%d align=%d | touch ctx ret=0x%08x n=%d "
	       "samp=0x%08x used=%u rej=%u last_n=%d | shown lvl=%d x=%d y=%d bad=0x%x resub=%u stick=%d,%d | vp mode=%d on=%d ret=0x%08x",
	       why, g_app_tid, g_fg_pid, g_armed, g_menu_open, g_level, g_x0, g_y0,
	       g_fb_w, g_fb_h, g_st.fb_pitch, g_st.fb_pf, g_st.fb_idx, g_st.setfb_calls,
	       g_st.zoom_calls, g_st.zoom_ret, g_zoom_fail, g_cfg.align_px,
	       g_st.ctx_ret, g_st.ctx_n, g_st.ctx_samp, g_st.smp_used, g_st.smp_rejected, g_st.last_n,
	       g_shown_level, g_shown_x, g_shown_y, g_lvl_bad, g_st.resubmits, g_st.stick_rx, g_st.stick_ry,
	       g_vp_mode, g_vp_active, g_vp_ret);
}

/* ---------- per-app profiles + remembered views ---------- */

/* last view of recently used apps, kept for this boot */
#define VIEW_CACHE 8
static struct { char tid[16]; int armed, w, x, y; uint32_t used; } views[VIEW_CACHE];
static uint32_t view_clock;

static void view_store(const char *tid)
{
	if (!tid[0]) return;
	int slot = 0;
	for (int i = 0; i < VIEW_CACHE; i++) {
		if (!strcmp(views[i].tid, tid)) { slot = i; break; }
		if (views[i].used < views[slot].used) slot = i;
	}
	snprintf(views[slot].tid, sizeof(views[slot].tid), "%s", tid);
	views[slot].armed = g_armed;
	views[slot].w = g_level > 0 ? vz_crop_w(g_level) : 0;
	views[slot].x = g_x0;
	views[slot].y = g_y0;
	views[slot].used = ++view_clock;
}

static int view_restore(const char *tid)
{
	for (int i = 0; i < VIEW_CACHE; i++) {
		if (!views[i].used || strcmp(views[i].tid, tid)) continue;
		g_armed = views[i].armed;
		if (views[i].w)
			vz_set_view(vz_level_for_width(views[i].w), views[i].x, views[i].y);
		return 1;
	}
	return 0;
}

static void switch_app(SceUID pid)
{
	char tid[16];
	memset(tid, 0, sizeof(tid));
	if (ksceKernelSysrootGetProcessTitleId(pid, tid, sizeof(tid) - 1) < 0 || !tid[0])
		snprintf(tid, sizeof(tid), "pid%x", pid);
	if (!strcmp(tid, g_app_tid))
		return; /* same app (e.g. it recreated its process): keep state */

	view_store(g_app_tid); /* remember how we leave the previous app */
	snprintf(g_app_tid, sizeof(g_app_tid), "%s", tid);
	vz_config_load_app(g_app_tid);
	vz_gesture_reset();
	vz_content_reset();
	vz_set_view(0, 0, 0);
	if (!view_restore(g_app_tid)) {
		g_armed = g_cfg.auto_arm;
		if (g_armed && g_cfg.view_w)
			vz_set_view(vz_level_for_width(g_cfg.view_w), g_cfg.view_x, g_cfg.view_y);
	}
	if (g_armed)
		vz_osd("VitaZoom: zoom on");
	vz_log("app: %s profile=%d auto_arm=%d", g_app_tid, g_app_profile, g_cfg.auto_arm);
}

/* ---------- worker ---------- */

static int combo_hit(uint32_t held, uint32_t pressed, uint32_t combo)
{
	return combo && (held & combo) == combo && (pressed & combo);
}

/* move the zoomed area with the right stick (fixed point, 1/16 px) */
static void stick_pan(const SceCtrlData *pad, uint32_t held)
{
	static int fx, fy, active;
	g_st.stick_rx = pad->rx;
	g_st.stick_ry = pad->ry;
	if (g_cfg.pan_stick == STICK_OFF || !g_armed || g_menu_open || g_level <= 0 ||
	    g_fg_pid != g_app_pid ||
	    (g_cfg.pan_stick == STICK_L_RIGHT && !(held & SCE_CTRL_LTRIGGER))) {
		active = 0;
		g_hide_rstick = 0;
		return;
	}
	g_hide_rstick = 1; /* the stick is ours while it moves the zoom */
	int dx = (int)pad->rx - 128, dy = (int)pad->ry - 128;
	const int dead = 30;
	dx = dx > dead ? dx - dead : dx < -dead ? dx + dead : 0;
	dy = dy > dead ? dy - dead : dy < -dead ? dy + dead : 0;
	if (!dx && !dy) { active = 0; return; }
	/* start from the current view (also after a touch drag moved it) */
	if (!active || fx / 16 != g_x0) fx = g_x0 * 16;
	if (!active || fy / 16 != g_y0) fy = g_y0 * 16;
	active = 1;
	/* same speed on screen at every zoom level / framebuffer size:
	 * full deflection ~ 12 screen px per tick (~700 px/s) */
	fx += dx * 2 * vz_crop_w(g_level) / SCREEN_W;
	fy += dy * 2 * vz_crop_h(g_level) / SCREEN_H;
	vz_set_view(g_level, fx / 16, fy / 16);
	if (g_x0 != fx / 16) fx = g_x0 * 16; /* hit an edge */
	if (g_y0 != fy / 16) fy = g_y0 * 16;
}

static int worker(SceSize args, void *argp)
{
	(void)args; (void)argp;
	uint32_t prev = 0, last_log = 0, last_render = 0, pid_since = 0;
	int last_armed = 0, last_level = 0, last_menu = 0;
	SceUID cand_pid = -1, cur_pid = -1;

	vz_config_load_global();
	ksceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	log_begin();
	int osd_level = 0;

	while (running) {
		SceCtrlData pad;
		memset(&pad, 0, sizeof(pad));
		ksceCtrlPeekBufferPositive(0, &pad, 1);
		uint32_t held = pad.buttons & VZ_BTN_MASK;
		uint32_t pressed = held & ~prev;
		uint32_t now = ksceKernelGetSystemTimeLow();

		/* looked up here, not in the display hook: the hooks only read the result */
		g_exclusive_running = ksceAppMgrIsExclusiveProcessRunning();

		/* foreground app changes (debounced: must hold for 0.5 s) */
		SceUID fg = g_fg_pid;
		static SceUID checked_pid = -2;
		if (fg != checked_pid) {
			checked_pid = fg;
			tai_module_info_t info;
			info.size = sizeof(info);
			int emu = fg >= 0 && taiGetModuleInfoForKernel(fg, "ScePspemu", &info) >= 0;
			g_pspemu_pid = emu ? fg : -1;
			if (emu)
				vz_log("PSP emulator in the foreground (pid=0x%x): compatibility mode", fg);
		}
		if (fg != cand_pid) {
			cand_pid = fg;
			pid_since = now;
		} else if (fg >= 0 && fg != cur_pid && now - pid_since > 500000 && !g_menu_open) {
			cur_pid = fg;
			switch_app(fg);
			g_app_pid = fg;
		}

		/* never leave the menu (and the button block) up behind the system:
		 * PS button overlay, or another app came to the front */
		if (g_menu_open && ((pad.buttons & SCE_CTRL_PSBUTTON) ||
		                    (fg != g_app_pid && fg == cand_pid && now - pid_since > 500000))) {
			vz_menu_close();
			g_block_buttons = 0;
			vz_log("menu closed: the system or another app came to the front");
		}

		if (g_menu_open) {
			vz_menu_input(held, pressed, now); /* the menu combo also closes it */
		} else if (combo_hit(held, pressed, g_cfg.menu_combo)) {
			vz_menu_open();
		} else if (combo_hit(held, pressed, g_cfg.toggle_combo)) {
			g_armed = !g_armed;
			vz_gesture_reset();
			if (!g_armed) vz_set_view(0, 0, 0);
			vz_osd(g_armed ? "VitaZoom: zoom on" : "VitaZoom: zoom off");
		}

		stick_pan(&pad, held);
		vz_view_apply(); /* viewport zoom follows the requested view */
		vz_snap_poll();
		/* the picture area changed (black bars found or gone): keep the
		 * view inside it */
		if (g_cont_changed) {
			g_cont_changed = 0;
			if (g_level > 0) vz_set_view(g_level, g_x0, g_y0);
		}

		/* show the zoom level while it changes (pinch, stick, double tap) */
		if (g_armed && !g_menu_open && g_level != osd_level) {
			char msg[32], name[16];
			vz_level_name(g_level, name, sizeof(name));
			snprintf(msg, sizeof(msg), "Zoom %s", name);
			vz_osd(msg);
		}
		osd_level = g_level;

		/* After the menu closes keep hiding buttons until all are released,
		 * so the closing press doesn't leak into the app. */
		if (!g_menu_open && g_block_buttons && !held)
			g_block_buttons = 0;

		if (g_menu_open && now - last_render > 250000) {
			vz_menu_render(); /* live diagnostics */
			last_render = now;
		}

		if (g_armed != last_armed || g_menu_open != last_menu) {
			log_state(g_armed != last_armed ? "armed" : "menu");
			last_armed = g_armed;
			last_menu = g_menu_open;
			last_level = g_level;
		} else if (g_level != last_level && now - last_log > 500000) {
			log_state("level");
			last_level = g_level;
			last_log = now;
		} else if ((g_armed || g_menu_open) && now - last_log > 5000000) {
			log_state("state");
			vz_log("  touch region=%d hits=%u,%u,%u,%u enable=0x%x | idx0=%u idx1=%u | picture %d,%d-%d,%d | restore tries %u",
			       g_st.touch_region, g_st.region_hits[0], g_st.region_hits[1],
			       g_st.region_hits[2], g_st.region_hits[3], g_st.touch_enable_ret,
			       g_st.idx_calls[0], g_st.idx_calls[1], g_cont_x0, g_cont_y0, g_cont_x1, g_cont_y1,
			       g_st.restore_tries);
			last_log = now;
		}

		prev = held;
		ksceKernelDelayThread(16 * 1000);
	}
	return 0;
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp)
{
	(void)args; (void)argp;
	vz_config_defaults();
	if (vz_zoom_init() < 0)
		return SCE_KERNEL_START_FAILED;

	running = 1;
	g_worker_thid = ksceKernelCreateThread("vitazoom", worker, 0x3C, 0x4000, 0, 0x10000, NULL);
	vz_hooks_init(); /* after the thread id is known (kernel ctrl bypass) */
	if (g_worker_thid >= 0)
		ksceKernelStartThread(g_worker_thid, 0, NULL);
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp)
{
	(void)args; (void)argp;
	running = 0;
	if (g_worker_thid >= 0) {
		ksceKernelWaitThreadEnd(g_worker_thid, NULL, NULL);
		ksceKernelDeleteThread(g_worker_thid);
	}
	vz_hooks_exit();
	vz_zoom_exit();
	return SCE_KERNEL_STOP_SUCCESS;
}
