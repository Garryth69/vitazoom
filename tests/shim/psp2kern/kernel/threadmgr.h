/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test shim: a clock the tests control. */
#pragma once
#include <psp2kern/types.h>
extern uint32_t test_now_us;
static inline uint32_t ksceKernelGetSystemTimeLow(void) { return test_now_us; }
static inline SceUID ksceKernelGetThreadId(void) { return 1; }
static inline SceUID ksceKernelGetProcessId(void) { return 0x100; }
