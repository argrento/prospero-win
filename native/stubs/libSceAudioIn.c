/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Link-only import facade; firmware supplies these implementations. */
#include <stdint.h>
#define U(x) ((void)(x))
int32_t sceAudioInOpen(int32_t u,int32_t t,int32_t i,uint32_t g,uint32_t r,uint32_t f)
{U(u);U(t);U(i);U(g);U(r);U(f);return -1;}
int32_t sceAudioInInput(int32_t h,void *p){U(h);U(p);return -1;}
int32_t sceAudioInGetSilentState(int32_t h){U(h);return -1;}
int32_t sceAudioInClose(int32_t h){U(h);return -1;}
