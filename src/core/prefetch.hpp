/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file prefetch.hpp Cache prefetch hints for loops over scattered memory. */

#ifndef PREFETCH_HPP
#define PREFETCH_HPP

#if defined(_MSC_VER) && !defined(__clang__) && (defined(_M_X64) || defined(_M_IX86))
#	include <xmmintrin.h>
#endif

/**
 * Hint the CPU to start loading the cache line holding \a p.
 * Purely a performance hint: it never faults (any address may be passed) and never changes state.
 * @param p Address that will be read soon.
 */
inline void PrefetchCacheLine(const void *p)
{
#if defined(__GNUC__) || defined(__clang__)
	__builtin_prefetch(p);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
	_mm_prefetch(static_cast<const char *>(p), _MM_HINT_T0);
#else
	(void)p;
#endif
}

/**
 * Hint the CPU to start loading all cache lines of an object.
 * @param p Start of the object.
 * @param size Size of the object in bytes.
 */
inline void PrefetchRange(const void *p, size_t size)
{
	const char *c = static_cast<const char *>(p);
	for (size_t offset = 0; offset < size; offset += 64) PrefetchCacheLine(c + offset);
}

#endif /* PREFETCH_HPP */
