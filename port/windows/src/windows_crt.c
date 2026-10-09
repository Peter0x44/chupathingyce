/*
WINDOWS_CRT.C

The C runtime file functions the game calls with Xbox paths, translated the
way CreateFile translates them (port/windows/include/crt/
halo_windows_file_names.h; the Linux build does the same in
port/linux/src/msvc_crt.c).
*/

#include "platform.h"

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <wchar.h>

static const char *translated(const char *path, char *buffer, size_t size)
{
	platform_translate_path(path, buffer, size);
	return buffer;
}

FILE *halo_windows_fopen(const char *path, const char *mode)
{
	char host_path[1024];

	return fopen(translated(path, host_path, sizeof(host_path)), mode);
}

FILE *halo_windows_freopen(const char *path, const char *mode, FILE *stream)
{
	char host_path[1024];

	return freopen(path ? translated(path, host_path, sizeof(host_path)) : NULL, mode, stream);
}

int halo_windows_remove(const char *path)
{
	char host_path[1024];

	return remove(translated(path, host_path, sizeof(host_path)));
}

int halo_windows_rename(const char *old_path, const char *new_path)
{
	char host_old_path[1024], host_new_path[1024];

	return rename(translated(old_path, host_old_path, sizeof(host_old_path)),
		translated(new_path, host_new_path, sizeof(host_new_path)));
}

int halo_windows_open(const char *path, int flags, ...)
{
	char host_path[1024];
	int mode = _S_IREAD | _S_IWRITE;

	if (flags & _O_CREAT)
	{
		va_list arguments;

		va_start(arguments, flags);
		mode = va_arg(arguments, int);
		va_end(arguments);
	}
	return _open(translated(path, host_path, sizeof(host_path)), flags | _O_NOINHERIT, mode);
}

int halo_windows_unlink(const char *path)
{
	char host_path[1024];

	return _unlink(translated(path, host_path, sizeof(host_path)));
}

int halo_windows_access(const char *path, int mode)
{
	char host_path[1024];

	/* Windows has no execute permission to test */
	return _access(translated(path, host_path, sizeof(host_path)), mode & 06);
}

int halo_windows_mkdir(const char *path)
{
	char host_path[1024];

	return _mkdir(translated(path, host_path, sizeof(host_path)));
}

int halo_windows_rmdir(const char *path)
{
	char host_path[1024];

	return _rmdir(translated(path, host_path, sizeof(host_path)));
}

#ifdef __MINGW32__
/* MSVC's compiler barrier, which the game declares and calls
(scenario/scenario.c); llvm-mingw has it only as a macro of <intrin.h>,
which halo_windows_prefix.h takes away. A call is itself a barrier. */
void _ReadWriteBarrier(void)
{
	__asm__ __volatile__("" ::: "memory");
}

/* _getws (text/unicode.c's ugets), which msvcrt.dll's import library for
32-bit ARM has not: a line of the console's input, without its newline */
wchar_t *_getws(wchar_t *buffer)
{
	size_t length;

	if (!fgetws(buffer, 1024, stdin))
		return NULL;
	length = wcslen(buffer);
	if (length && buffer[length - 1] == L'\n')
		buffer[length - 1] = 0;
	return buffer;
}
#endif
