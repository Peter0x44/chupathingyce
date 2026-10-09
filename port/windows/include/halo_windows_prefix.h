/*
HALO_WINDOWS_PREFIX.H

Force-included ahead of every game unit and every Xbox-facing platform unit
in the native Windows build (clang -include); the Windows-facing units
(port/windows/src/win32_*.c) never see it. The Microsoft target already
provides most of the MSVC environment the game was written for, so this is
much shorter than the Linux build's halo_linux_prefix.h.
*/

#ifndef __HALO_WINDOWS_PREFIX_H
#define __HALO_WINDOWS_PREFIX_H

/* (32-bit ARM: the Windows RT build, whose llvm-mingw has __arm__ rather
than _M_ARM) */
#if !(defined(_M_IX86) || defined(__arm__)) && !((defined(_M_X64) || defined(_M_ARM64)) && defined(HALO_64BIT))
#error the Windows port targets 32-bit x86 or ARM, or x64 or ARM64 with HALO_64BIT: game data structures assume 32-bit pointers
#endif

#define HALO_WINDOWS 1

/* ---------- llvm-mingw (the Windows RT build)

mingw's target is a GNU one: its C inlines are not MSVC's COMDATs, and its
headers spell __forceinline as an extern GNU inline. As the Linux build
does (halo_linux_prefix.h): the C runtime's headers first, before the
redefinitions below can reach them (their include guards keep later
includes from seeing them), then every inline function static, and those
that also have an ordinary prototype weak (the generated
halo_msvc_tags.h's #pragma weak). <limits.h> is left out: cseries.h
defines LONG_MAX and friends as enumerators; <stdlib.h> too (libtiff
declares its malloc itself, and stdlib's own inlines are static). */

#ifdef __MINGW32__
#include <_mingw.h>
#include <stddef.h>
#include <stdarg.h>
#include <float.h>
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <assert.h>
/* (SDL's headers include it: ahead of the _Interlocked* names below) */
#include <intrin.h>
/* (a macro of intrin.h's; the game declares it a function, which
windows_crt.c defines) */
#undef _ReadWriteBarrier
#undef __forceinline
#define __inline static __inline__
#define _inline static __inline__
#define __forceinline static __inline__ __attribute__((always_inline))
/* A COFF weak definition's default is named after the unit's first public
symbol, which two units can share (an Xbox SDK table, an auto-import
reference), and the link then refuses the two defaults: each unit's first
symbol is its own, named after it (HALO_UNIT, tools/windows_build.py). */
#ifdef HALO_UNIT
void HALO_UNIT(void);
void HALO_UNIT(void)
{
}
#endif
#endif

/* ---------- pointers inside Xbox data (the 64-bit builds': ninja windows64;
no-ops in the 32-bit build, as in the Linux build: the shared sources name
them) */

#include "../../../source/cseries/xbox_address.h"

/* ---------- the C runtime functions the game calls undeclared

The 64-bit build rejects an implicitly declared function, whose int result
would truncate a pointer (tools/windows_build.py); the 32-bit build lets the
game call these without their headers, as MSVC did. */

#ifdef HALO_64BIT
#include <ctype.h>
/* (text/unicode.c's ugets: the C runtime still has it, but no longer
declares it) */
wchar_t *__cdecl _getws(wchar_t *buffer);
#endif

/* ---------- XDK architecture selection */

/* (not for 32-bit ARM, whose mingw headers would take it for x86) */
#ifndef __arm__
#define _X86_ 1
#endif
#define _STDCALL_SUPPORTED 1
#define _USE_MATH_DEFINES

/* ---------- MSVC inline semantics

Clang's Microsoft target gives C `__inline` functions MSVC's COMDAT linkage
already. Where MSVC left a copy that clang inlines away, the build supplies
one: port/linux/game/msvc_comdat.c for header inlines, generated wrappers for
the few defined in .c files (tools/windows_build.py). */

/* ---------- MSVC intrinsics

Clang predeclares the MSVC _Interlocked* builtins with prototypes that
conflict with the XDK's WINAPI declarations; route the XDK names to the
platform layer instead (as on Linux: port/linux/src/xbox_kernel.c). */

#define _InterlockedCompareExchange halo_linux_InterlockedCompareExchange
#define _InterlockedDecrement halo_linux_InterlockedDecrement
#define _InterlockedExchange halo_linux_InterlockedExchange
#define _InterlockedExchangeAdd halo_linux_InterlockedExchangeAdd
#define _InterlockedIncrement halo_linux_InterlockedIncrement

/* ---------- structured exception handling

Only the top-level crash handler in main() uses SEH, and clang does not
catch a fault raised in the same function as the __try. The guarded block
always runs and the handler is compiled out; the platform layer reports
crashes instead (port/windows/src/win32_memory_watch.c). */

#define __try if (1)
#define __except(filter) else if (0)
#define __finally
#define __leave

/* ---------- multiplayer session limits of the native builds */

#include "../../linux/include/halo_port_limits.h"

/* the Xbox Winsock headers' fd_set, for game units and the platform's XNet
alike (the Windows-facing units never see this header) */
#define FD_SETSIZE HALO_PORT_FD_SETSIZE

/* ---------- Xbox functions named like Windows functions */

#include "halo_windows_api_names.h"

/* ---------- Winsock and source fixups shared with the Linux build */

#ifndef HALO_LINUX_PLATFORM_LAYER
/* the game's own strnlen (source/cseries/cseries.c), which the C runtime
also defines (see crt/string.h) */
#define strnlen halo_game_strnlen
#include "../../linux/include/halo_linux_winsock_names.h"
#include "../../linux/include/halo_linux_source_fixups.h"
#endif

#include <stddef.h>

#endif /* __HALO_WINDOWS_PREFIX_H */
