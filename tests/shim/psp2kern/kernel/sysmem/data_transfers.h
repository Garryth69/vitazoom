/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <string.h>
static inline int ksceKernelCopyToUser(void *dst, const void *src, size_t n) { memcpy(dst, src, n); return 0; }
static inline int ksceKernelCopyFromUser(void *dst, const void *src, size_t n) { memcpy(dst, src, n); return 0; }
