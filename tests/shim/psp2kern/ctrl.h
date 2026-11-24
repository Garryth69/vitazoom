/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test shim: button bits as in the VitaSDK (psp2common/ctrl.h). */
#pragma once
#include <psp2kern/types.h>
enum {
	SCE_CTRL_SELECT = 0x1, SCE_CTRL_L3 = 0x2, SCE_CTRL_R3 = 0x4, SCE_CTRL_START = 0x8,
	SCE_CTRL_UP = 0x10, SCE_CTRL_RIGHT = 0x20, SCE_CTRL_DOWN = 0x40, SCE_CTRL_LEFT = 0x80,
	SCE_CTRL_LTRIGGER = 0x100, SCE_CTRL_RTRIGGER = 0x200, SCE_CTRL_L1 = 0x400, SCE_CTRL_R1 = 0x800,
	SCE_CTRL_TRIANGLE = 0x1000, SCE_CTRL_CIRCLE = 0x2000, SCE_CTRL_CROSS = 0x4000,
	SCE_CTRL_SQUARE = 0x8000, SCE_CTRL_PSBUTTON = 0x10000,
};
