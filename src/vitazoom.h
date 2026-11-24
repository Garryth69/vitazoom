/* SPDX-License-Identifier: GPL-3.0-or-later
 * VitaZoom - pinch-to-zoom kernel plugin for PS Vita (taiHEN)
 * Copyright (C) 2026 VitaZoom contributors
 */
#ifndef VITAZOOM_H
#define VITAZOOM_H

#include <psp2kern/types.h>
#include <stdint.h>

#define VZ_VERSION "1.0.0-beta.1" /* SemVer, see README */

#define SCREEN_W 960
#define SCREEN_H 544
#define PANEL_W  1920 /* front touch panel coordinates */
#define PANEL_H  1088

/* Zoom levels as visible size in framebuffer pixels, largest first, in
 * fine steps (~4.6%, 960 down to 240) so a pinch feels continuous on any
 * framebuffer size (a 480x272 one still gets 16 steps). Level 0 = no zoom.
 * A level is usable when it is smaller than the app's framebuffer; sizes the
 * display refuses are dropped at runtime (720x408, 640x368 and 480x272 are
 * always accepted). At most 32 levels (g_lvl_bad is a bit mask). */
#define VZ_LEVELS 32
extern const int vz_lvl_w[VZ_LEVELS];
extern const int vz_lvl_h[VZ_LEVELS];
extern volatile uint32_t g_lvl_bad; /* bit per level refused by the display */

/* ---- configuration ---- */
enum { TOUCH_BLOCK, TOUCH_PASS };             /* touch_mode */
enum { STICK_OFF, STICK_RIGHT, STICK_L_RIGHT }; /* pan_stick */
enum { PINCH_SOFT, PINCH_NORMAL, PINCH_STRONG }; /* pinch */

typedef struct {
	/* global only */
	uint32_t menu_combo;    /* opens/closes the overlay menu */
	uint32_t toggle_combo;  /* arms/disarms zoom without the menu (0 = off) */
	int align_px;           /* horizontal crop alignment (pixels, auto-raised) */
	/* per app (profile overrides global defaults) */
	int auto_arm;           /* arm zoom when this app comes to the foreground */
	int touch_mode;         /* 1 finger: blocked, or passed corrected by zoom */
	int pan_stick;          /* move the zoomed area with the right stick */
	int dtap_enable;        /* double tap resets to 1x */
	int hold_step;          /* zoom per long-press step, x100 (125/150/200) */
	int pinch;              /* pinch strength: PINCH_SOFT/NORMAL/STRONG */
	int block_rear;         /* also hide rear touch from the app while armed */
	int view_w, view_x, view_y; /* saved view (crop width, 0 = none) */
} vz_config_t;

extern vz_config_t g_cfg;
extern char g_app_tid[16];         /* title id of the foreground app */
extern volatile int g_app_profile; /* 1 = app has its own profile */

void vz_config_defaults(void);
int vz_config_load_global(void);
int vz_config_save_global(void);
void vz_config_load_app(const char *tid); /* global defaults + app overrides */
int vz_config_save_app(const char *tid);
int vz_config_delete_app(const char *tid);

/* ---- shared runtime state ---- */
extern volatile int g_armed;          /* zoom mode active */
extern volatile int g_menu_open;
extern volatile int g_block_buttons;  /* hide buttons from apps */
extern volatile int g_level;          /* 0 = 1x */
extern volatile int g_x0, g_y0;       /* requested crop origin (fb pixels) */
extern volatile int g_shown_level;    /* what the display is showing now */
extern volatile int g_shown_x, g_shown_y;
extern volatile int g_zoom_fail;      /* display refused cropped framebuffer */
extern volatile SceUID g_fg_pid;      /* process owning the displayed framebuffer */
extern volatile SceUID g_app_pid;     /* process whose settings/zoom are loaded */
extern volatile int g_hide_rstick;    /* right stick moves the zoom: hide it from the app */
extern volatile int g_fb_w, g_fb_h;   /* its framebuffer size */
extern volatile int g_redraw;         /* overlay/view changed: re-present */
extern volatile int g_touch_latch;    /* multi-finger gesture in progress */
extern SceUID g_worker_thid;
/* Set by the worker, read by the hooks (never looked up in the display path):
 * the PSP emulator process (ScePspemu: Adrenaline and official PSP games),
 * and whether an exclusive app runs under SceShell. */
extern volatile SceUID g_pspemu_pid;
extern volatile int g_exclusive_running;

/* ---- levels.c (pure) + zoom.c ---- */
int vz_zoom_init(void);
void vz_zoom_exit(void);
int vz_level_ok(int level);           /* usable on the current framebuffer */
int vz_crop_w(int level);
int vz_crop_h(int level);
int vz_factor(int level);             /* x1000 relative to the framebuffer */
int vz_level_step(int level, int dir);/* next usable level in a direction */
int vz_max_level(void);
int vz_level_for_width(int w);        /* closest usable level for a crop width */
void vz_level_name(int level, char *buf, int len);
int vz_lvl_documented(int level);      /* always accepted by the display */
void vz_set_view(int level, int x0, int y0);
void vz_view_apply(void);             /* worker: push the view to the display */
extern volatile int g_vp_mode, g_vp_active, g_vp_ret;
/* Own frame for the menu on screens the app rewrites without presenting
 * (Adrenaline "Original"): allocated only while the menu is open. */
extern volatile int g_snap_on;
int vz_snap_begin(void);              /* worker, on menu open: 1 = own frame */
int vz_snap_visible(void);            /* own frame requested or still on screen */
void vz_view_off(void);               /* worker: drop the viewport immediately */
void vz_snap_end(void);               /* worker, on menu close */
void vz_snap_poll(void);              /* worker: frees the frame once unused, logs */

/* ---- hooks.c ---- */
int vz_hooks_init(void);
void vz_hooks_exit(void);
extern volatile SceUID g_self_peek_thid; /* our own touch read: don't filter */

/* ---- gesture.c ---- */
typedef struct {
	int n;           /* fingers (0..2 used) */
	int x[2], y[2];  /* screen coordinates 0..959 / 0..543 */
} vz_touch_t;
void vz_gesture_reset(void);
/* Arbitrated entry point for every touch sample source. Only samples from the
 * foreground process and newer than the last one are used. */
void vz_touch_sample(SceUID pid, uint64_t ts, int n, const int16_t *xy, int stride);

/* touch sample as returned by SceTouch (user and kernel APIs) */
typedef struct {
	uint8_t id, force;
	int16_t x, y;
	uint8_t reserved[8];
	uint16_t info;
} vz_report_t;

typedef struct {
	uint64_t timeStamp;
	uint32_t status;
	uint32_t reportNum;
	vz_report_t report[8];
} vz_tdata_t;

/* ---- diagnostics ---- */

typedef struct {
	uint32_t setfb_calls;
	int fb_idx, fb_pitch, fb_pf;
	uint32_t zoom_calls;
	int zoom_ret;
	int ctx_ret, ctx_n, ctx_samp;   /* touch read inside the app's context */
	uint32_t smp_used, smp_rejected;
	int last_n;
	uint32_t resubmits;
	uint32_t idx_calls[2];         /* setfb calls per framebuffer index */
	int touch_enable_ret;
	int touch_region;              /* -1 = plain touch read */
	uint32_t region_hits[4];
	int stick_rx, stick_ry;
	int ctrl_k_hooks, ctrl_u_hooks, touch_k_hooks, touch_u_hooks, vblank_hooks;
} vz_stats_t;
extern volatile vz_stats_t g_st;
void vz_log(const char *fmt, ...);

/* ---- menu.c ---- */
void vz_menu_open(void);
void vz_menu_close(void);
void vz_menu_input(uint32_t held, uint32_t pressed, uint32_t now_us);
void vz_menu_render(void);
void vz_osd(const char *msg);
const char *vz_combo_str(uint32_t combo, char *buf, int len);

/* ---- draw.c: overlays are display lists rasterised row by row while
 * being copied into the framebuffer (no pixel buffers). ---- */
#define MENU_W 640
#define MENU_H 544
#define OSD_W  400
#define OSD_H  36

enum { BG_PANEL, BG_SEL, BG_HEAD, BG_OSD };                              /* 2 bits */
enum { FG_TEXT, FG_WHITE, FG_DIM, FG_ACCENT, FG_OK, FG_WARN, FG_LINE };  /* 3 bits */
enum { DL_MENU, DL_OSD };

void vz_dl_begin(int which);
void vz_dl_size(int w, int h);        /* override the list size (compact menu) */
void vz_fill(int x, int y, int w, int h, int bg);
int vz_text_big(int x, int y, const char *s, int fg);   /* returns end x */
int vz_text_small(int x, int y, const char *s, int fg);
void vz_dl_commit(void);
int vz_text_len(const char *s);       /* characters in a UTF-8 string */
/* inv: overlay px per framebuffer px, x1024 (>= 1024; OSD shrinks with zoom) */
void vz_blit_overlay(void *fb_base, int pitch, int fb_w, int fb_h, int ox, int oy, int inv);
void vz_blit_menu_kernel(void *fb, int pitch, int fb_w, int fb_h);
extern volatile uint32_t g_menu_gen;  /* bumps on every menu list commit */
void vz_osd_show(uint32_t us);
int vz_osd_visible(void);             /* OSD still within its display time */

#define BIG_W 12
#define BIG_H 24

/* buttons we care about */
#define VZ_BTN_MASK 0x0000FFF9u /* everything except L3/R3 */

#endif
