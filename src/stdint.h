// Copyright (C) 2026 weibangtuo and other contributors
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 51
// Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#ifndef CCACHE_STDINT_H
#define CCACHE_STDINT_H

// Fallback <stdint.h> for platforms whose libc lacks the C99 header, notably
// AIX 5.1/5.2 which only provide <inttypes.h>. This file is picked up via the
// compiler's -I switches (see all_cppflags in Makefile.in) before the system
// include directories. On all other platforms the system header is used via
// #include_next.

#if defined(_AIX51) || defined(_AIX52)

// AIX 5.x <inttypes.h> already provides all fixed-width types, the limit
// macros and the *_C constants needed here (the ISO-C part is exposed when
// _ALL_SOURCE is defined, which GCC defines by default on AIX). Redefining
// the types ourselves would conflict with AIX's definitions (e.g. the fast
// types differ), so only include the system header and add the few things it
// lacks.
#include <inttypes.h>

#ifndef SIZE_MAX
#define SIZE_MAX __SIZE_MAX__
#endif

#else // !(_AIX51 || _AIX52)

#include_next <stdint.h>

#endif

#endif // ifndef CCACHE_STDINT_H
