/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test shim: the SDK file API mapped onto POSIX, rooted in a temp dir. */
#pragma once
#include <psp2kern/types.h>
enum { SCE_O_RDONLY = 0x1, SCE_O_WRONLY = 0x2, SCE_O_APPEND = 0x100, SCE_O_CREAT = 0x200, SCE_O_TRUNC = 0x400 };
SceUID ksceIoOpen(const char *path, int flags, int mode);
int ksceIoRead(SceUID fd, void *buf, SceSize n);
int ksceIoWrite(SceUID fd, const void *buf, SceSize n);
int ksceIoClose(SceUID fd);
int ksceIoMkdir(const char *path, int mode);
int ksceIoRemove(const char *path);
