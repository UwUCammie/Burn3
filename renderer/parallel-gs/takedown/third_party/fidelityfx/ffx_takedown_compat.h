// Force-included into the vendored FidelityFX sources (see README.takedown.md).
// Vulkan calls go through Granite's volk loader, and the MSVC "secure" CRT
// calls the SDK uses are supplied here under their own names, on every
// platform: glibc has none of them and MinGW declares only some, depending on
// the language mode.
#pragma once

#ifdef __cplusplus
#include <volk.h>

#include <cmath>
#include <codecvt>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <locale>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifdef _WIN32
// Every system header the SDK includes comes in before the renames below, so
// none of their own declarations is renamed.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <FidelityFX/host/ffx_util.h>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

inline int takedown_ffx_wcscpy_s(wchar_t *dst, size_t size, const wchar_t *src)
{
	if (!dst || size == 0)
		return 1;
	std::wcsncpy(dst, src ? src : L"", size - 1);
	dst[size - 1] = L'\0';
	return 0;
}
template <size_t N>
inline int takedown_ffx_wcscpy_s(wchar_t (&dst)[N], const wchar_t *src)
{
	return takedown_ffx_wcscpy_s(dst, N, src);
}
inline int takedown_ffx_strcpy_s(char *dst, size_t size, const char *src)
{
	if (!dst || size == 0)
		return 1;
	std::strncpy(dst, src ? src : "", size - 1);
	dst[size - 1] = '\0';
	return 0;
}
inline int takedown_ffx_sprintf_s(char *buffer, size_t size, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	const int result = std::vsnprintf(buffer, size, format, args);
	va_end(args);
	return result;
}
inline int takedown_ffx_swprintf_s(wchar_t *buffer, size_t size, const wchar_t *format, ...)
{
	va_list args;
	va_start(args, format);
	const int result = std::vswprintf(buffer, size, format, args);
	va_end(args);
	return result;
}
inline int takedown_ffx_wcstombs_s(size_t *converted, char *dst, size_t size, const wchar_t *src, size_t count)
{
	if (!dst || size == 0)
		return 1;
	const size_t limit = count < size ? count : size - 1;
	const size_t result = std::wcstombs(dst, src, limit);
	if (result == size_t(-1))
	{
		dst[0] = '\0';
		if (converted)
			*converted = 0;
		return 1;
	}
	dst[result < size ? result : size - 1] = '\0';
	if (converted)
		*converted = result + 1;
	return 0;
}
#define wcscpy_s takedown_ffx_wcscpy_s
#define strcpy_s takedown_ffx_strcpy_s
#define sprintf_s takedown_ffx_sprintf_s
#define swprintf_s takedown_ffx_swprintf_s
#define wcstombs_s takedown_ffx_wcstombs_s
#endif
