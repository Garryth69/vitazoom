/* SPDX-License-Identifier: GPL-3.0-or-later
 * VitaZoom - pinch-to-zoom kernel plugin for PS Vita (taiHEN)
 * Copyright (C) 2026 VitaZoom contributors
 */
/*
 * zoom.c - display side: zoom, overlay drawing, touch sampling.
 *
 * Zoom uses the display controller viewport (ksceDisplaySetViewportConf), the
 * same mechanism as the system Accessibility zoom: the hardware scales a part
 * of whatever is on screen, so it works for every app (Adrenaline included),
 * costs no CPU/GPU time and moves smoothly. If the display refuses it, zoom
 * falls back to framebuffer cropping in the SetFrameBuf hook (base moved to
 * the crop origin, 64 px horizontal steps).
 *
 * The SetFrameBuf hook runs in the context of the presenting process, which
 * is also where touch is delivered (touch goes to the focused process), so
 * the touch panel is sampled here: gestures work even in apps that never
 * read it. The overlay (menu/OSD) is drawn into the frame here as well.
 *
 * Apps that present rarely (static screens, Adrenaline "Original" which
 * presents its VRAM once) are handled from the vblank-wait syscalls: their
 * last frame is presented again when needed, and the menu is shown on our
 * own frame (see "own frame" below).
 */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysmem/memtype.h>
#include <psp2kern/display.h>
#include <taihen.h>
#include "vitazoom.h"

int ksceAppMgrIsExclusiveProcessRunning(void);

/* Display-controller viewport (newer vita-headers; this is what the system
 * Accessibility zoom uses). Coordinates are in a 960x544 frame, independent
 * of the app's framebuffer, x <= 640 and y <= 364 on FW 3.60. */
typedef struct {
	SceSize size; /* 0x14 */
	unsigned int x, y, width, height;
} vz_viewport_t;
int ksceDisplaySetViewportConf(int head, int fb_idx, const vz_viewport_t *conf);

/* SceTouchForDriver (not declared in the SDK headers) */
int ksceTouchPeek(uint32_t port, vz_tdata_t *data, uint32_t nbufs);
int ksceTouchPeekRegion(uint32_t port, vz_tdata_t *data, uint32_t nbufs, int region);
int ksceTouchSetSamplingState(uint32_t port, int state);
int ksceTouchSetEnableFlag(uint32_t port, int enable);


volatile int g_shown_level = 0, g_shown_x = 0, g_shown_y = 0;
volatile int g_zoom_fail = 0;
volatile SceUID g_fg_pid = -1;
volatile SceUID g_app_pid = -1;
volatile int g_hide_rstick = 0;
volatile SceUID g_pspemu_pid = -1;
volatile int g_exclusive_running = 0;
volatile int g_fb_w = SCREEN_W, g_fb_h = SCREEN_H;
volatile int g_redraw = 0;

/* zoom method: viewport (preferred) or framebuffer crop (fallback) */
volatile int g_vp_mode = 1;   /* 1 = use the viewport while it works */
volatile int g_vp_active = 0; /* a viewport is currently applied */
volatile int g_vp_ret = 0;    /* last result */
static int vp_slot = -1, vp_level = -1, vp_x = -1, vp_y = -1, vp_fbw, vp_fbh;

static tai_hook_ref_t ref_setfb;
static SceUID hook_setfb = -1;
static SceUID sampling_pid = -1;

/* last frame presented by the foreground app (for re-presenting) */
static SceDisplayFrameBuf last_fb;
static int last_head, last_index;
static SceUID last_pid = -1;
static volatile uint32_t last_flip_t;
static volatile SceUID resubmit_thid = -1;

/* ---- own frame for the menu ----
 * Apps like Adrenaline ("Original" filtering) present their framebuffer once
 * and keep rewriting it, so anything drawn into it is erased at once and it
 * may be small (480x272, shown 2x). While the menu is open on such a screen
 * we show our own 960x544 frame instead: a dimmed still of the app plus the
 * full-size menu. The memory exists only while the menu is open. */
#define SNAP_W 960
#define SNAP_H 544
#define SNAP_SIZE 0x200000
volatile int g_snap_on = 0;
static SceUID snap_uid = -1;
static uint32_t *snap_base;
static volatile int snap_need_capture, snap_shown, snap_index = -1, snap_busy;
static volatile SceUID snap_pid = -1; /* app whose display shows our frame */
static volatile uint32_t snap_gen_drawn, snap_off_t;
static volatile int snap_refused_ret; /* logged by the worker, not in the hook */
static int snap_type_first; /* 1 = main RAM was refused by the display: use CDRAM */
static uint32_t snap_row[SNAP_W];

static int app_stalled(void)
{
	return last_pid >= 0 && last_pid == g_fg_pid &&
	       ksceKernelGetSystemTimeLow() - last_flip_t > 100000;
}

static void vp_reset(void);

int vz_snap_visible(void)
{
	return g_snap_on || (snap_shown && snap_pid == g_fg_pid);
}

/* worker: zoom off right now (before the menu shows up) */
void vz_view_off(void)
{
	if (g_vp_active) { vp_reset(); g_shown_level = 0; }
}

int vz_snap_begin(void)
{
	g_snap_on = 0;
	if (!app_stalled() || last_fb.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 ||
	    !last_fb.width || last_fb.width > SNAP_W || !last_fb.height || last_fb.height > SNAP_H)
		return 0;
	if (snap_uid < 0) {
		static const SceKernelMemBlockType types[] = {
			SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_NC_RW, /* main RAM */
			SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_CDRAM_RW, /* GPU memory: last resort */
		};
		for (unsigned i = snap_type_first; i < sizeof(types) / sizeof(types[0]) && snap_uid < 0; i++) {
			SceUID uid = ksceKernelAllocMemBlock("vitazoom_menu", types[i], SNAP_SIZE, NULL);
			void *base = NULL;
			if (uid >= 0 && ksceKernelGetMemBlockBase(uid, &base) >= 0 && base) {
				snap_uid = uid;
				snap_base = base;
			} else if (uid >= 0) {
				ksceKernelFreeMemBlock(uid);
			}
			vz_log("menu: own frame type=0x%08x ret=0x%08x", types[i], uid);
		}
		if (snap_uid < 0) return 0;
	}
	snap_need_capture = 1;
	snap_gen_drawn = g_menu_gen - 1;
	g_snap_on = 1;
	return 1;
}

void vz_snap_end(void)
{
	if (!g_snap_on) return;
	g_snap_on = 0;
	snap_off_t = ksceKernelGetSystemTimeLow();
	g_redraw = 1; /* put the app's own frame back */
}

void vz_snap_poll(void)
{
	if (snap_refused_ret) {
		vz_log("menu: own frame refused 0x%08x%s", snap_refused_ret,
		       snap_type_first ? "" : ": next time in video memory");
		snap_refused_ret = 0;
		snap_type_first = 1; /* the refused block is freed below, retried as CDRAM */
	}
	/* free only after the display has switched back to an app frame */
	if (g_snap_on || snap_uid < 0 || snap_shown || snap_busy) return;
	if (ksceKernelGetSystemTimeLow() - snap_off_t < 200000) return;
	ksceKernelFreeMemBlock(snap_uid);
	snap_uid = -1;
	snap_base = NULL;
}

/* app context: dimmed still of the app's last frame, scaled to 960x544 */
static void snap_capture(void)
{
	int fw = last_fb.width, fh = last_fb.height, pitch = last_fb.pitch;
	int prev_sy = -1, ok = 0;
	for (int y = 0; y < SNAP_H; y++) {
		uint32_t *dst = snap_base + y * SNAP_W;
		int sy = y * fh / SNAP_H;
		if (sy == prev_sy) {
			memcpy(dst, dst - SNAP_W, SNAP_W * 4);
			continue;
		}
		prev_sy = sy;
		ok = ksceKernelCopyFromUser(snap_row, (const uint32_t *)last_fb.base + sy * pitch,
		                            fw * 4) >= 0;
		for (int x = 0; x < SNAP_W; x++) {
			uint32_t c = ok ? snap_row[x * fw / SNAP_W] : 0;
			dst[x] = 0xFF000000 | ((c >> 2) & 0x3F3F3F); /* 25% brightness */
		}
	}
}

/* app context: present our frame (with the current menu) instead of the app's */
static int snap_present(int head, int index, int sync)
{
	if (snap_need_capture) {
		snap_capture();
		snap_need_capture = 0;
	}
	uint32_t gen = g_menu_gen;
	if (gen != snap_gen_drawn) {
		vz_blit_menu_kernel(snap_base, SNAP_W, SNAP_W, SNAP_H);
		snap_gen_drawn = gen;
	}
	SceDisplayFrameBuf fb;
	memset(&fb, 0, sizeof(fb));
	fb.size = sizeof(fb);
	fb.base = snap_base;
	fb.pitch = SNAP_W;
	fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	fb.width = SNAP_W;
	fb.height = SNAP_H;
	int ret = TAI_CONTINUE(int, ref_setfb, head, index, &fb, sync);
	if (ret >= 0) {
		snap_shown = 1;
		snap_index = index;
		snap_pid = ksceKernelGetProcessId();
	} else {
		snap_refused_ret = ret ? ret : -1;
		g_snap_on = 0; /* back to drawing into the app's frame */
		snap_off_t = ksceKernelGetSystemTimeLow(); /* worker re-renders it compact */
	}
	return ret;
}

/* ---- viewport zoom (called by the worker every tick) ---- */

static void vp_reset(void)
{
	if (vp_slot >= 0)
		ksceDisplaySetViewportConf(0, vp_slot, NULL);
	vp_slot = vp_level = vp_x = vp_y = -1;
	g_vp_active = 0;
}

void vz_view_apply(void)
{
	/* only on the app the zoom belongs to (not on the LiveArea or the next
	 * app while the switch is being confirmed) */
	int want = g_vp_mode && g_armed && !g_menu_open && g_level > 0 &&
	           g_fb_w > 0 && g_fb_h > 0 && g_fg_pid == g_app_pid &&
	           !vz_snap_visible(); /* never zoom our own menu frame */
	int slot = g_st.fb_idx & 1;
	if (!want) {
		if (g_vp_active) { vp_reset(); g_shown_level = 0; g_redraw = 1; }
		return;
	}
	int level = g_level, x0 = g_x0, y0 = g_y0;
	/* nothing changed: still re-sent once a second, so the zoom comes back by
	 * itself if the display was reset (suspend/resume, app video mode change) */
	static uint32_t sent_t;
	uint32_t now = ksceKernelGetSystemTimeLow();
	if (g_vp_active && slot == vp_slot && level == vp_level && x0 == vp_x && y0 == vp_y &&
	    g_fb_w == vp_fbw && g_fb_h == vp_fbh && now - sent_t < 1000000)
		return;
	sent_t = now;
	if (g_vp_active && slot != vp_slot) vp_reset();

	for (int tries = 0; tries < VZ_LEVELS; tries++) {
		int w = vz_crop_w(level), h = vz_crop_h(level);
		vz_viewport_t vp;
		vp.size = sizeof(vp);
		vp.x = (unsigned)(x0 * SCREEN_W / g_fb_w);
		vp.y = (unsigned)(y0 * SCREEN_H / g_fb_h);
		vp.width = (unsigned)(w * SCREEN_W / g_fb_w);
		vp.height = (unsigned)(h * SCREEN_H / g_fb_h);
		int ret = ksceDisplaySetViewportConf(0, slot, &vp);
		g_vp_ret = ret;
		if (ret >= 0) {
			vp_slot = slot; vp_level = level; vp_x = x0; vp_y = y0;
			vp_fbw = g_fb_w; vp_fbh = g_fb_h;
			g_vp_active = 1;
			g_shown_level = level;
			g_shown_x = x0;
			g_shown_y = y0;
			g_zoom_fail = 0;
			g_redraw = 1;
			return;
		}
		if (vz_lvl_documented(level)) {
			/* even a plain size is refused: no viewport here, use the crop */
			vz_log("viewport refused (0x%08x) at %ux%u+%u+%u: using the crop",
			       ret, vp.width, vp.height, vp.x, vp.y);
			g_vp_mode = 0;
			vp_reset();
			return;
		}
		/* refused size: drop this level, try the nearest one */
		g_lvl_bad |= 1u << level;
		level = vz_level_for_width(vz_lvl_w[level]);
		if (level <= 0) break;
		g_level = level;
		vz_set_view(level, x0, y0);
		x0 = g_x0; y0 = g_y0;
	}
	vp_reset();
}

/* ---- display hook ---- */

static int zoomable(const SceDisplayFrameBuf *p)
{
	return (p->pixelformat == SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 ||
	        p->pixelformat == SCE_DISPLAY_PIXELFORMAT_A2B10G10R10) &&
	       p->pitch >= p->width && (int)p->width > vz_lvl_w[VZ_LEVELS - 1] &&
	       (int)p->height > vz_lvl_h[VZ_LEVELS - 1];
}

static void sample_touch_here(SceUID pid)
{
	if (sampling_pid != pid) {
		/* make sure the panel is sampled for this process */
		g_st.ctx_samp = ksceTouchSetSamplingState(0, 1);
		sampling_pid = pid;
	}
	/* Some apps (e.g. Adrenaline) get touch only through touch regions:
	 * when the plain read stays empty, probe regions 0..3 in turn and stick
	 * with one that reports fingers. */
	static int probe, empty_frames;
	static SceUID enabled_pid = -1, empty_pid = -1;
	if (empty_pid != pid) { empty_pid = pid; empty_frames = 0; }
	vz_tdata_t td;
	td.reportNum = 0;
	g_self_peek_thid = ksceKernelGetThreadId();
	int ret, region = g_st.touch_region;
	if (region >= 0) {
		ret = ksceTouchPeekRegion(0, &td, 1, region);
	} else {
		ret = ksceTouchPeek(0, &td, 1);
		if (ret >= 0 && td.reportNum == 0 && ++empty_frames > 30) {
			/* the panel may be disabled for this app: enable it once. Never in the
			 * PSP emulator: it keeps its panel off on purpose, and changing the
			 * emulator's state from here is not ours to do */
			if (empty_frames == 31 && enabled_pid != pid && pid != g_pspemu_pid) {
				g_st.touch_enable_ret = ksceTouchSetEnableFlag(0, 1);
				enabled_pid = pid;
			}
			vz_tdata_t tr;
			tr.reportNum = 0;
			int r = probe++ & 3;
			int rr = ksceTouchPeekRegion(0, &tr, 1, r);
			if (rr > 0 && tr.reportNum > 0) {
				g_st.region_hits[r]++;
				if (g_st.region_hits[r] >= 3) g_st.touch_region = r;
				td = tr;
				ret = rr;
			}
		} else if (td.reportNum > 0) {
			empty_frames = 0;
		}
	}
	g_self_peek_thid = -1;
	g_st.ctx_ret = ret;
	if (ret <= 0) return;
	int n = td.reportNum > 2 ? 2 : (int)td.reportNum;
	g_st.ctx_n = n;
	int16_t xy[4];
	for (int i = 0; i < n; i++) {
		xy[i * 2] = td.report[i].x;
		xy[i * 2 + 1] = td.report[i].y;
	}
	vz_touch_sample(pid, td.timeStamp, n, xy, 2);
}

/* aligned x for the crop, with hysteresis so the view doesn't flicker
 * between two positions when the requested x sits near a boundary */
static int pick_x(int x0, int w, int fb_w, int align, int level)
{
	static int prev_ax = -1, prev_level = -1;
	int maxx = fb_w - w;
	int ax = (x0 + align / 2) / align * align;
	if (level == prev_level && prev_ax >= 0) {
		int d = x0 - prev_ax;
		if (d < 0) d = -d;
		if (d < align * 3 / 4) ax = prev_ax;
	}
	while (ax > maxx) ax -= align;
	if (ax < 0) ax = 0;
	prev_ax = ax;
	prev_level = level;
	return ax;
}

static int setfb_hook(int head, int index, const SceDisplayFrameBuf *p, int sync)
{
	if (!p || !p->base || p->size < sizeof(SceDisplayFrameBuf) ||
	    head != ksceDisplayGetPrimaryHead())
		return TAI_CONTINUE(int, ref_setfb, head, index, p, sync);

	SceUID pid = ksceKernelGetProcessId();
	/* SceShell drawing on top of a running app (PS button menu, notices):
	 * leave it alone. Decided by caller process, not by index, so apps like
	 * Adrenaline (ScePspemu) are handled like any other app. */
	if (pid == ksceKernelSysrootGetShellPid() && g_exclusive_running)
		return TAI_CONTINUE(int, ref_setfb, head, index, p, sync);

	if (g_fg_pid != pid) g_st.touch_region = -1;
	g_fg_pid = pid;
	if ((int)p->width != g_fb_w || (int)p->height != g_fb_h) {
		g_fb_w = p->width;
		g_fb_h = p->height;
		g_lvl_bad = 0;
		vz_set_view(g_level, g_x0, g_y0); /* re-validate for the new size */
	}
	g_st.setfb_calls++;
	g_st.idx_calls[index & 1]++;
	g_st.fb_idx = index;
	g_st.fb_pitch = p->pitch;
	g_st.fb_pf = p->pixelformat;

	/* remember the original frame for re-presenting */
	if (ksceKernelGetThreadId() != resubmit_thid) {
		memcpy(&last_fb, p, sizeof(last_fb));
		last_head = head;
		last_index = index;
		last_pid = pid;
		last_flip_t = ksceKernelGetSystemTimeLow();
	}
	g_redraw = 0;

	if (g_snap_on && pid == last_pid) {
		__sync_add_and_fetch(&snap_busy, 1);
		int r = g_snap_on && snap_base ? snap_present(head, index, sync) :
		        TAI_CONTINUE(int, ref_setfb, head, index, p, sync);
		__sync_sub_and_fetch(&snap_busy, 1);
		return r;
	}
	if (snap_shown && index == snap_index && p->base != snap_base)
		snap_shown = 0; /* the display is back on an app frame */

	if (g_armed && !g_menu_open)
		sample_touch_here(pid);

	/* Overlay: the menu over the full frame; the OSD only on frames the app
	 * presents itself (it redraws them, so the OSD disappears with it; on a
	 * re-presented static frame it would stay burned in or, on a live VRAM,
	 * flicker). */
	int real = ksceKernelGetThreadId() != resubmit_thid;
	if (p->pixelformat == SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) {
		if (g_menu_open) {
			vz_blit_overlay(p->base, p->pitch, p->width, p->height, 0, 0, 1024);
		} else if (real && vz_osd_visible()) {
			/* inside the visible area, shrunk so it keeps its size on screen
			 * whatever the zoom or framebuffer size */
			int ox = 0, oy = 0, inv = 1024, vis_w = (int)p->width;
			if (g_shown_level > 0) {
				ox = g_shown_x;
				oy = g_shown_y;
				vis_w = vz_crop_w(g_shown_level);
			}
			if (vis_w > 0 && vis_w < SCREEN_W) inv = SCREEN_W * 1024 / vis_w;
			vz_blit_overlay(p->base, p->pitch, p->width, p->height, ox, oy, inv);
		}
	}

	if (g_vp_mode || g_vp_active)
		return TAI_CONTINUE(int, ref_setfb, head, index, p, sync);
	if (!g_armed || g_menu_open || g_level <= 0 || !zoomable(p)) {
		g_shown_level = 0;
		return TAI_CONTINUE(int, ref_setfb, head, index, p, sync);
	}

	for (int tries = 0; tries < VZ_LEVELS + 4; tries++) {
		int level = g_level;
		if (level <= 0 || !vz_level_ok(level)) break;
		int w = vz_lvl_w[level], h = vz_lvl_h[level];
		int y0 = g_y0;
		if (y0 > (int)p->height - h) y0 = p->height - h;
		if (y0 < 0) y0 = 0;
		int align = g_cfg.align_px > 0 ? g_cfg.align_px : 1;
		int ax = pick_x(g_x0, w, p->width, align, level);

		SceDisplayFrameBuf z;
		memcpy(&z, p, sizeof(z));
		z.size = sizeof(z);
		z.base = (char *)p->base + ((uint32_t)y0 * p->pitch + (uint32_t)ax) * 4;
		z.width = w;
		z.height = h;
		int ret = TAI_CONTINUE(int, ref_setfb, head, index, &z, sync);
		g_st.zoom_calls++;
		g_st.zoom_ret = ret;
		if (ret >= 0) {
			g_zoom_fail = 0;
			g_shown_level = level;
			g_shown_x = ax;
			g_shown_y = y0;
			return ret;
		}
		if (align < 64) {
			/* refused address: try a coarser alignment */
			g_cfg.align_px = align < 8 ? 8 : align * 2;
		} else if (!vz_lvl_documented(level)) {
			/* refused size: drop this level for good, use the nearest one */
			g_lvl_bad |= 1u << level;
			g_level = vz_level_for_width(w);
		} else {
			g_zoom_fail = 1;
			break;
		}
	}
	g_shown_level = 0;
	return TAI_CONTINUE(int, ref_setfb, head, index, p, sync);
}

/* ---- re-present on vblank waits (apps that rarely present) ---- */

static void maybe_resubmit(void)
{
	if (last_pid < 0) return;
	SceUID pid = ksceKernelGetProcessId();
	if (pid != last_pid || pid != g_fg_pid) return;
	uint32_t now = ksceKernelGetSystemTimeLow();
	if (now - last_flip_t < 100000) return; /* the app is presenting by itself */

	/* static screen: keep gestures alive (~100 Hz max). Only reads the touch
	 * panel, so it is also done in the emulator: in "Original" it presents
	 * almost never and this is the only place its touch is read. */
	static volatile uint32_t last_sample_t;
	if (g_armed && !g_menu_open && now - last_sample_t > 10000) {
		last_sample_t = now;
		sample_touch_here(pid);
	}
	/* The PSP emulator presents its own frames (and its "Original" mode scans the
	 * PSP display out without presenting): a frame of ours from one of its threads
	 * races with the emulator's own switches (menu, savestate, PS1) and can leave
	 * an old picture on screen. Only while our menu is up, or until the display is
	 * back on the emulator's frame after it. */
	if (pid == g_pspemu_pid && !g_menu_open && !g_snap_on && !snap_shown) return;
	static uint32_t last_resubmit_t;
	/* menu closed but our frame is still on screen: the app's frame must come
	 * back (the emulator in "Original" never presents again by itself) */
	int restore = !g_snap_on && snap_shown;
	if (g_snap_on) {
		/* our frame stays on screen by itself: present only when it changed */
		if (snap_shown && !snap_need_capture && g_menu_gen == snap_gen_drawn) return;
	} else if (!restore && !g_redraw && !g_menu_open) return;
	if (now - last_resubmit_t < (g_menu_open ? 15000u : 33000u)) return;
	last_resubmit_t = now;
	if (!__sync_bool_compare_and_swap(&resubmit_thid, -1, ksceKernelGetThreadId()))
		return;
	SceDisplayFrameBuf fb;
	memcpy(&fb, &last_fb, sizeof(fb));
	ksceDisplaySetFrameBufInternal(last_head, last_index, &fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
	g_st.resubmits++;
	resubmit_thid = -1;
}

#define VB_LIB 0x5ED8F994 /* SceDisplay (user) */
enum { VB_START, VB_START_CB, VB_MULTI, VB_MULTI_CB, VB_COUNT };
static const uint32_t vb_nids[VB_COUNT] = {0x5795E898, 0x78B41B92, 0xDD0A13B8, 0x05F27764};
static tai_hook_ref_t vb_refs[VB_COUNT];
static SceUID vb_hooks[VB_COUNT];

static int vb_hook_0(void) { maybe_resubmit(); return TAI_CONTINUE(int, vb_refs[0]); }
static int vb_hook_1(void) { maybe_resubmit(); return TAI_CONTINUE(int, vb_refs[1]); }
static int vb_hook_2(unsigned int v) { maybe_resubmit(); return TAI_CONTINUE(int, vb_refs[2], v); }
static int vb_hook_3(unsigned int v) { maybe_resubmit(); return TAI_CONTINUE(int, vb_refs[3], v); }
static const void *const vb_funcs[VB_COUNT] = {vb_hook_0, vb_hook_1, vb_hook_2, vb_hook_3};

int vz_zoom_init(void)
{
	hook_setfb = taiHookFunctionExportForKernel(KERNEL_PID, &ref_setfb,
		"SceDisplay", 0x9FED47AC /* SceDisplayForDriver */,
		0x16466675 /* ksceDisplaySetFrameBufInternal */, setfb_hook);
	if (hook_setfb < 0) return hook_setfb;
	g_st.vblank_hooks = 0;
	for (int i = 0; i < VB_COUNT; i++) {
		vb_hooks[i] = taiHookFunctionExportForKernel(KERNEL_PID, &vb_refs[i],
			"SceDisplay", VB_LIB, vb_nids[i], vb_funcs[i]);
		if (vb_hooks[i] >= 0) g_st.vblank_hooks++;
	}
	g_st.touch_region = -1;
	return 0;
}

void vz_zoom_exit(void)
{
	vp_reset();
	g_snap_on = 0;
	for (int i = 0; i < VB_COUNT; i++)
		if (vb_hooks[i] >= 0)
			taiHookReleaseForKernel(vb_hooks[i], vb_refs[i]);
	if (hook_setfb >= 0)
		taiHookReleaseForKernel(hook_setfb, ref_setfb);
	hook_setfb = -1;
}
