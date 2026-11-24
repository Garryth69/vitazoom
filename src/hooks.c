/* SPDX-License-Identifier: GPL-3.0-or-later
 * VitaZoom - pinch-to-zoom kernel plugin for PS Vita (taiHEN)
 * Copyright (C) 2026 VitaZoom contributors
 */
/*
 * hooks.c - filter what games see.
 *  - Buttons/sticks are neutralised while the menu is open, both on the
 *    user SceCtrl API (native games) and on the kernel SceCtrlForDriver API
 *    (used by kernel-side readers such as Adrenaline/ScePspemu helpers).
 *  - Front touch (and optionally rear) is hidden while zoom is armed, so
 *    pinch/drag gestures never reach the game. Touch samples seen here are
 *    also fed to the gesture engine (primary source when the game polls).
 */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/ctrl.h>
#include <taihen.h>
#include "vitazoom.h"

/* ---------- user ctrl ---------- */

#define CTRL_LIB     0xD197E3C7 /* SceCtrl (user) */
#define CTRL_DRV_LIB 0x7823A5D1 /* SceCtrlForDriver */

enum {
	C_PEEK_P, C_READ_P, C_PEEK_N, C_READ_N,
	C_PEEK_P2, C_READ_P2, C_PEEK_N2, C_READ_N2,
	C_PEEK_PE, C_READ_PE, C_PEEK_PE2, C_READ_PE2,
	C_COUNT
};

static const uint32_t ctrl_nids[C_COUNT] = {
	0xA9C3CED6, 0x67E7AB83, 0x104ED1A7, 0x15F96FB0,
	0x15F81E8C, 0xC4226A3E, 0x81A89660, 0x27A0C5FB,
	0xA59454D3, 0xE2D99296, 0x860BF292, 0xA7178860,
};
static const int ctrl_negative[C_COUNT] = {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0};

static tai_hook_ref_t ctrl_refs[C_COUNT];
static SceUID ctrl_hooks[C_COUNT];

static void blank_one(SceCtrlData *k, int negative)
{
	k->buttons = negative ? 0xFFFFFFFFu : 0;
	k->lx = k->ly = k->rx = k->ry = 0x80;
}

/* all buttons and sticks (menu open), or only the right stick (it moves
 * the zoom) */
static void filter_pad(SceCtrlData *k, int negative)
{
	if (g_block_buttons) blank_one(k, negative);
	else k->rx = k->ry = 0x80;
}

static int pads_filtered(void)
{
	return g_block_buttons || (g_hide_rstick && ksceKernelGetProcessId() == g_app_pid);
}

static void filter_user_pads(SceCtrlData *upad, int count, int negative)
{
	if (count > 64) count = 64;
	for (int i = 0; i < count; i++) {
		SceCtrlData k;
		if (ksceKernelCopyFromUser(&k, &upad[i], sizeof(k)) < 0)
			return;
		filter_pad(&k, negative);
		ksceKernelCopyToUser(&upad[i], &k, sizeof(k));
	}
}

#define CTRL_HOOK(idx)                                                        \
	static int ctrl_hook_##idx(int port, SceCtrlData *pad, int count)     \
	{                                                                     \
		int ret = TAI_CONTINUE(int, ctrl_refs[idx], port, pad, count); \
		if (ret > 0 && pad && pads_filtered())                        \
			filter_user_pads(pad, ret, ctrl_negative[idx]);       \
		return ret;                                                   \
	}

CTRL_HOOK(0) CTRL_HOOK(1) CTRL_HOOK(2) CTRL_HOOK(3)
CTRL_HOOK(4) CTRL_HOOK(5) CTRL_HOOK(6) CTRL_HOOK(7)
CTRL_HOOK(8) CTRL_HOOK(9) CTRL_HOOK(10) CTRL_HOOK(11)

static const void *const ctrl_funcs[C_COUNT] = {
	ctrl_hook_0, ctrl_hook_1, ctrl_hook_2, ctrl_hook_3,
	ctrl_hook_4, ctrl_hook_5, ctrl_hook_6, ctrl_hook_7,
	ctrl_hook_8, ctrl_hook_9, ctrl_hook_10, ctrl_hook_11,
};

/* ---------- kernel ctrl (Adrenaline & other kernel readers) ---------- */

enum { K_PEEK_P, K_PEEK_N, K_READ_P, K_READ_N, K_COUNT };
static const uint32_t kctrl_nids[K_COUNT] = {
	0xEA1D3A34, 0x19895843, 0x9B96A1AA, 0x8D4E0DD1,
};
static const int kctrl_negative[K_COUNT] = {0, 1, 0, 1};
static tai_hook_ref_t kctrl_refs[K_COUNT];
static SceUID kctrl_hooks[K_COUNT];

#define KCTRL_HOOK(idx)                                                        \
	static int kctrl_hook_##idx(int port, SceCtrlData *pad, int count)     \
	{                                                                      \
		int ret = TAI_CONTINUE(int, kctrl_refs[idx], port, pad, count); \
		/* our own worker must keep seeing the real buttons */         \
		if (ret > 0 && pad && g_block_buttons &&                       \
		    ksceKernelGetThreadId() != g_worker_thid) {                \
			int c = ret > 64 ? 64 : ret;                           \
			for (int i = 0; i < c; i++)                            \
				blank_one(&pad[i], kctrl_negative[idx]);       \
		}                                                              \
		return ret;                                                    \
	}

KCTRL_HOOK(0) KCTRL_HOOK(1) KCTRL_HOOK(2) KCTRL_HOOK(3)

static const void *const kctrl_funcs[K_COUNT] = {
	kctrl_hook_0, kctrl_hook_1, kctrl_hook_2, kctrl_hook_3,
};

/* ---------- touch ---------- */

#define TOUCH_LIB 0x3E4F4A81 /* SceTouch (user) */


volatile SceUID g_self_peek_thid = -1;

static void feed(uint32_t port, const vz_tdata_t *k)
{
	if (port != 0 || !g_armed || g_menu_open) return;
	int16_t xy[4];
	int n = k->reportNum > 2 ? 2 : (int)k->reportNum;
	for (int i = 0; i < n; i++) {
		xy[i * 2] = k->report[i].x;
		xy[i * 2 + 1] = k->report[i].y;
	}
	vz_touch_sample(ksceKernelGetProcessId(), k->timeStamp, n, xy, 2);
}

/* Decide what the app gets for one buffer. Returns 1 if *k was changed. */
static int filter_one(uint32_t port, vz_tdata_t *k)
{
	if (g_menu_open) goto block;
	if (!g_armed) return 0;
	if (port == 1) {
		if (g_cfg.block_rear) goto block;
		return 0;
	}
	if (port != 0) return 0;
	if (g_cfg.touch_mode == TOUCH_BLOCK || g_touch_latch || k->reportNum >= 2)
		goto block;
	/* corrected touch: map the finger from the zoomed view to the frame */
	int l = g_shown_level;
	if (k->reportNum == 1 && l > 0 && g_fb_w > 0 && g_fb_h > 0) {
		int w = vz_lvl_w[l], h = vz_lvl_h[l];
		int px = k->report[0].x, py = k->report[0].y;
		int fx = g_shown_x + px * w / PANEL_W;
		int fy = g_shown_y + py * h / PANEL_H;
		k->report[0].x = (int16_t)(fx * PANEL_W / g_fb_w);
		k->report[0].y = (int16_t)(fy * PANEL_H / g_fb_h);
		return 1;
	}
	return 0;
block:
	if (k->reportNum == 0) return 0;
	k->reportNum = 0;
	return 1;
}

/* kernel API: buffers are accessed directly (same as ds4vita/VitaControl) */
static void handle_k(uint32_t port, vz_tdata_t *d, int count)
{
	if (count <= 0 || !d) return;
	if (ksceKernelGetThreadId() == g_self_peek_thid) return; /* our own read */
	if (count > 64) count = 64;
	feed(port, &d[count - 1]);
	for (int i = 0; i < count; i++)
		filter_one(port, &d[i]);
}

/* user API: buffers live in user memory */
static void handle_u(uint32_t port, vz_tdata_t *ud, int count)
{
	if (count <= 0 || !ud) return;
	if (count > 64) count = 64;
	if (!g_armed && !g_menu_open) return;
	for (int i = 0; i < count; i++) {
		vz_tdata_t k;
		if (ksceKernelCopyFromUser(&k, &ud[i], sizeof(k)) < 0)
			return;
		if (i == count - 1)
			feed(port, &k);
		if (filter_one(port, &k))
			ksceKernelCopyToUser(&ud[i], &k, sizeof(k));
	}
}

enum { T_PEEK, T_READ, T_PEEK_R, T_READ_R, T_COUNT };
static const uint32_t utouch_nids[T_COUNT] = {
	0xFF082DF0, 0x169A1D58, 0x04440622, 0x93B5635B,
};
static const uint32_t ktouch_nids[T_COUNT] = {
	0xBAD1960B, 0x70C8AACE, 0x9B3F7207, 0x9A91F624,
};
static tai_hook_ref_t utouch_refs[T_COUNT], ktouch_refs[T_COUNT];
static SceUID utouch_hooks[T_COUNT], ktouch_hooks[T_COUNT];

#define TOUCH_HOOK3(pfx, idx, handler)                                         \
	static int pfx##_hook_##idx(uint32_t port, vz_tdata_t *d, uint32_t n)  \
	{                                                                      \
		int ret = TAI_CONTINUE(int, pfx##_refs[idx], port, d, n);      \
		handler(port, d, ret);                                         \
		return ret;                                                    \
	}
#define TOUCH_HOOK4(pfx, idx, handler)                                         \
	static int pfx##_hook_##idx(uint32_t port, vz_tdata_t *d, uint32_t n,  \
	                            int region)                                \
	{                                                                      \
		int ret = TAI_CONTINUE(int, pfx##_refs[idx], port, d, n, region); \
		handler(port, d, ret);                                         \
		return ret;                                                    \
	}

TOUCH_HOOK3(utouch, 0, handle_u) TOUCH_HOOK3(utouch, 1, handle_u)
TOUCH_HOOK4(utouch, 2, handle_u) TOUCH_HOOK4(utouch, 3, handle_u)
TOUCH_HOOK3(ktouch, 0, handle_k) TOUCH_HOOK3(ktouch, 1, handle_k)
TOUCH_HOOK4(ktouch, 2, handle_k) TOUCH_HOOK4(ktouch, 3, handle_k)

static const void *const utouch_funcs[T_COUNT] = {
	utouch_hook_0, utouch_hook_1, utouch_hook_2, utouch_hook_3,
};
static const void *const ktouch_funcs[T_COUNT] = {
	ktouch_hook_0, ktouch_hook_1, ktouch_hook_2, ktouch_hook_3,
};

/* ---------- install ---------- */

int vz_hooks_init(void)
{
	g_st.ctrl_u_hooks = g_st.ctrl_k_hooks = 0;
	g_st.touch_u_hooks = g_st.touch_k_hooks = 0;
	for (int i = 0; i < C_COUNT; i++) {
		ctrl_hooks[i] = taiHookFunctionExportForKernel(KERNEL_PID, &ctrl_refs[i],
			"SceCtrl", CTRL_LIB, ctrl_nids[i], ctrl_funcs[i]);
		if (ctrl_hooks[i] >= 0) g_st.ctrl_u_hooks++;
	}
	for (int i = 0; i < K_COUNT; i++) {
		kctrl_hooks[i] = taiHookFunctionExportForKernel(KERNEL_PID, &kctrl_refs[i],
			"SceCtrl", CTRL_DRV_LIB, kctrl_nids[i], kctrl_funcs[i]);
		if (kctrl_hooks[i] >= 0) g_st.ctrl_k_hooks++;
	}
	for (int i = 0; i < T_COUNT; i++) {
		utouch_hooks[i] = taiHookFunctionExportForKernel(KERNEL_PID, &utouch_refs[i],
			"SceTouch", TOUCH_LIB, utouch_nids[i], utouch_funcs[i]);
		if (utouch_hooks[i] >= 0) g_st.touch_u_hooks++;
		ktouch_hooks[i] = taiHookFunctionExportForKernel(KERNEL_PID, &ktouch_refs[i],
			"SceTouch", TAI_ANY_LIBRARY, ktouch_nids[i], ktouch_funcs[i]);
		if (ktouch_hooks[i] >= 0) g_st.touch_k_hooks++;
	}
	return 0;
}

void vz_hooks_exit(void)
{
	for (int i = 0; i < C_COUNT; i++)
		if (ctrl_hooks[i] >= 0)
			taiHookReleaseForKernel(ctrl_hooks[i], ctrl_refs[i]);
	for (int i = 0; i < K_COUNT; i++)
		if (kctrl_hooks[i] >= 0)
			taiHookReleaseForKernel(kctrl_hooks[i], kctrl_refs[i]);
	for (int i = 0; i < T_COUNT; i++) {
		if (utouch_hooks[i] >= 0)
			taiHookReleaseForKernel(utouch_hooks[i], utouch_refs[i]);
		if (ktouch_hooks[i] >= 0)
			taiHookReleaseForKernel(ktouch_hooks[i], ktouch_refs[i]);
	}
}
