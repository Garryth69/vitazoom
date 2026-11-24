/* SPDX-License-Identifier: GPL-3.0-or-later
 * VitaZoom - pinch-to-zoom kernel plugin for PS Vita (taiHEN)
 * Copyright (C) 2026 VitaZoom contributors
 */
/*
 * config.c - settings.
 *   ur0:data/vitazoom/config.txt            global settings + defaults
 *   ur0:data/vitazoom/apps/<TITLEID>.txt    per-app overrides
 * Format: key=value per line, '#' comments.
 */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/iofilemgr.h>
#include <psp2kern/ctrl.h>
#include "vitazoom.h"

#define CFG_DIR  "ur0:data/vitazoom"
#define CFG_PATH CFG_DIR "/config.txt"
#define APP_DIR  CFG_DIR "/apps"

vz_config_t g_cfg;
static vz_config_t g_global; /* global defaults, before app overrides */
char g_app_tid[16];
volatile int g_app_profile;

void vz_config_defaults(void)
{
	g_cfg.menu_combo = SCE_CTRL_DOWN | SCE_CTRL_SELECT;
	g_cfg.toggle_combo = 0;
	g_cfg.align_px = 64; /* the display wants 256-byte aligned bases */
	g_cfg.auto_arm = 0;
	g_cfg.touch_mode = TOUCH_BLOCK;
	g_cfg.pan_stick = STICK_OFF;
	g_cfg.dtap_enable = 1;
	g_cfg.hold_step = 150;
	g_cfg.pinch = PINCH_NORMAL;
	g_cfg.block_rear = 0;
	g_cfg.view_w = g_cfg.view_x = g_cfg.view_y = 0;
	g_global = g_cfg;
}

static uint32_t parse_num(const char *s)
{
	uint32_t v = 0;
	int base = 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
	for (;; s++) {
		int d;
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
		else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
		else break;
		v = v * base + d;
	}
	return v;
}

static void apply(vz_config_t *c, const char *k, const char *v, int global)
{
	uint32_t n = parse_num(v);
	if (global) {
		if (!strcmp(k, "menu_combo") && (n & VZ_BTN_MASK)) c->menu_combo = n & VZ_BTN_MASK;
		else if (!strcmp(k, "toggle_combo")) c->toggle_combo = n & VZ_BTN_MASK;
		else if (!strcmp(k, "align") && n >= 1 && n <= 64) c->align_px = n;
	}
	if (!strcmp(k, "auto_arm")) c->auto_arm = !!n;
	else if (!strcmp(k, "touch_mode") && n <= TOUCH_PASS) c->touch_mode = n;
	else if (!strcmp(k, "pan_stick") && n <= STICK_L_RIGHT) c->pan_stick = n;
	else if (!strcmp(k, "dtap_enable")) c->dtap_enable = !!n;
	else if (!strcmp(k, "hold_step") && (n == 125 || n == 150 || n == 200)) c->hold_step = n;
	else if (!strcmp(k, "block_rear")) c->block_rear = !!n;
	else if (!strcmp(k, "pinch") && n <= PINCH_STRONG) c->pinch = n;
	else if (!strcmp(k, "view_w") && (n == 0 || (n >= 200 && n < 960))) c->view_w = n;
	else if (!strcmp(k, "view_x") && n < 960) c->view_x = n;
	else if (!strcmp(k, "view_y") && n < 544) c->view_y = n;
}

static int load_file(const char *path, vz_config_t *c, int global)
{
	char buf[1024];
	SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0) return fd;
	int len = ksceIoRead(fd, buf, sizeof(buf) - 1);
	ksceIoClose(fd);
	if (len <= 0) return -1;
	buf[len] = 0;

	char *line = buf;
	while (line && *line) {
		char *next = strchr(line, '\n');
		if (next) *next++ = 0;
		char *eq = strchr(line, '=');
		if (eq && line[0] != '#') {
			*eq = 0;
			char *v = eq + 1;
			char *end = v + strlen(v);
			while (end > v && (end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
			apply(c, line, v, global);
		}
		line = next;
	}
	return 0;
}

static int write_file(const char *path, const char *buf, int len)
{
	SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0) return fd;
	int w = ksceIoWrite(fd, buf, len);
	ksceIoClose(fd);
	return w == len ? 0 : -1;
}

static int fmt_app_keys(char *buf, int size, const vz_config_t *c)
{
	return snprintf(buf, size,
		"auto_arm=%d\n"
		"touch_mode=%d\n"
		"pan_stick=%d\n"
		"dtap_enable=%d\n"
		"hold_step=%d\n"
		"block_rear=%d\n"
		"pinch=%d\n",
		c->auto_arm, c->touch_mode, c->pan_stick, c->dtap_enable, c->hold_step,
		c->block_rear, c->pinch);
}

static void app_path(char *buf, int len, const char *tid)
{
	snprintf(buf, len, APP_DIR "/%s.txt", tid);
}

int vz_config_load_global(void)
{
	int r = load_file(CFG_PATH, &g_cfg, 1);
	g_global = g_cfg;
	return r;
}

int vz_config_save_global(void)
{
	char buf[512];
	int len = snprintf(buf, sizeof(buf),
		"# VitaZoom - global settings (default for all apps)\n"
		"menu_combo=0x%08X\n"
		"toggle_combo=0x%08X\n"
		"align=%d\n",
		g_cfg.menu_combo, g_cfg.toggle_combo, g_cfg.align_px);
	len += fmt_app_keys(buf + len, sizeof(buf) - len, &g_cfg);
	ksceIoMkdir(CFG_DIR, 0777);
	int r = write_file(CFG_PATH, buf, len);
	if (r >= 0) g_global = g_cfg;
	return r;
}

void vz_config_load_app(const char *tid)
{
	/* global settings that are not per-app stay as they are now */
	vz_config_t c = g_global;
	c.menu_combo = g_cfg.menu_combo;
	c.toggle_combo = g_cfg.toggle_combo;
	c.align_px = g_cfg.align_px;
	c.view_w = c.view_x = c.view_y = 0;
	char path[64];
	app_path(path, sizeof(path), tid);
	g_app_profile = load_file(path, &c, 0) >= 0;
	g_cfg = c;
}

int vz_config_save_app(const char *tid)
{
	char buf[384], path[64];
	int len = snprintf(buf, sizeof(buf), "# VitaZoom - profile for %s\n", tid);
	len += fmt_app_keys(buf + len, sizeof(buf) - len, &g_cfg);
	/* the current zoom becomes this app's starting view */
	g_cfg.view_w = (g_armed && g_level > 0) ? vz_crop_w(g_level) : 0;
	g_cfg.view_x = g_x0;
	g_cfg.view_y = g_y0;
	len += snprintf(buf + len, sizeof(buf) - len, "view_w=%d\nview_x=%d\nview_y=%d\n",
	                g_cfg.view_w, g_cfg.view_x, g_cfg.view_y);
	ksceIoMkdir(CFG_DIR, 0777);
	ksceIoMkdir(APP_DIR, 0777);
	app_path(path, sizeof(path), tid);
	int r = write_file(path, buf, len);
	if (r >= 0) g_app_profile = 1;
	return r;
}

int vz_config_delete_app(const char *tid)
{
	char path[64];
	app_path(path, sizeof(path), tid);
	int r = ksceIoRemove(path);
	g_app_profile = 0;
	vz_config_load_app(tid); /* back to the global defaults */
	return r;
}
