/*
 * Copyright (c) 2023 Lain Bailey <lain@obsproject.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define BMEM_TRACE

#include <stdlib.h>
#include <string.h>
#include "base.h"
#include "bmem.h"
#include "platform.h"
#include "threading.h"

/*
 * NOTE: totally jacked the mem alignment trick from ffmpeg, credit to them:
 *   http://www.ffmpeg.org/
 */

#ifdef BMEM_TRACE
#include <execinfo.h>
#endif

#define ALIGNMENT 32

/*
 * Attention, intrepid adventurers, exploring the depths of the libobs code!
 *
 * There used to be a TODO comment here saying that we should use memalign on
 * non-Windows platforms. However, since *nix/POSIX systems do not provide an
 * aligned realloc(), this is currently not (easily) achievable.
 * So while the use of posix_memalign()/memalign() would be a fairly trivial
 * change, it would also ruin our memory alignment for some reallocated memory
 * on those platforms.
 */
#if defined(_WIN32)
#define ALIGNED_MALLOC 1
#else
#define ALIGNMENT_HACK 1
#endif

#ifdef BMEM_TRACE
#define BMEM_TRACE_DEPTH 9

struct bmem_trace {
	struct bmem_trace *next;
	struct bmem_trace **prev_next;
	void *buffer[BMEM_TRACE_DEPTH];
	int nptrs;
	size_t size;
};

#define BMEM_TRACE_SIZE_BYTE \
	((sizeof(struct bmem_trace) + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT)
#define BMEM_OVERRUN_TEST_BYTE ALIGNMENT

#define BMEM_OVERRUN_TEST_CODE 0xB3

static struct bmem_trace *trace_first = NULL;
static pthread_mutex_t bmem_trace_mutex = PTHREAD_MUTEX_INITIALIZER;

static void bmem_trace_dump_once(int log_level, struct bmem_trace *bt);

static inline void register_trace(void *ptr, size_t size)
{
	struct bmem_trace *bt = ptr;
	bt->nptrs = backtrace(bt->buffer, BMEM_TRACE_DEPTH);
	bt->size = size;

	pthread_mutex_lock(&bmem_trace_mutex);
	bt->prev_next = &trace_first;
	bt->next = trace_first;
	trace_first = bt;
	if (bt->next)
		bt->next->prev_next = &bt->next;
	pthread_mutex_unlock(&bmem_trace_mutex);
}

static void unregister_trace(void *ptr)
{
	struct bmem_trace *bt = ptr;
	pthread_mutex_lock(&bmem_trace_mutex);
	if (*bt->prev_next != bt) {
		blog(LOG_ERROR,
		     "unregister_trace corrupted *prev_next=%p expected %p prev_next: %p next: %p",
		     *bt->prev_next, bt, bt->prev_next, bt->next);
		bmem_trace_dump_once(LOG_ERROR, bt);
		if (bt->next)
			bmem_trace_dump_once(LOG_ERROR, bt->next);
	}
	*bt->prev_next = bt->next;
	if (bt->next)
		bt->next->prev_next = bt->prev_next;
	pthread_mutex_unlock(&bmem_trace_mutex);
}

static void reregister_trace(void *ptr, size_t size)
{
	struct bmem_trace *bt = ptr;
	bt->size = size;
	pthread_mutex_lock(&bmem_trace_mutex);
	if (bt->next)
		bt->next->prev_next = &bt->next;
	*bt->prev_next = bt;
	pthread_mutex_unlock(&bmem_trace_mutex);
}

static void bmem_trace_dump_once(int log_level, struct bmem_trace *bt)
{
	int nptrs = bt->nptrs;
	if (nptrs <= 0 || nptrs > BMEM_TRACE_DEPTH) {
		blog(LOG_ERROR, "backtrace buffer broken %p nptrs=%d", bt,
		     bt->nptrs);
		nptrs = BMEM_TRACE_DEPTH;
	}
	char **strings = backtrace_symbols(bt->buffer, nptrs);
	if (!strings) {
		blog(LOG_ERROR, "backtrace_symbols for memory returns NULL");
	} else {
		for (int i = 0; i < nptrs; i++) {
			blog(log_level, "memory leak trace[%d]: %s", i,
			     strings[i]);
		}
		free(strings);
	}
}

void bmem_trace_dump(int log_level)
{
	pthread_mutex_lock(&bmem_trace_mutex);
	int n = 0;
	for (struct bmem_trace *bt = trace_first; bt; bt = bt->next) {
		blog(log_level, "memory leak[%d] %p", n, bt);
		bmem_trace_dump_once(log_level, bt);
		n++;
	}
	pthread_mutex_unlock(&bmem_trace_mutex);
}

static void bmem_overrun_test_set(uint8_t *ptr)
{
	for (size_t i = 0; i < BMEM_OVERRUN_TEST_BYTE; i++)
		ptr[i] = BMEM_OVERRUN_TEST_CODE + i;
}

static void bmem_overrun_test_check(uint8_t *ptr)
{
	bool pass = true;
	for (size_t i = 0; i < BMEM_OVERRUN_TEST_BYTE; i++)
		pass &= ptr[i] == BMEM_OVERRUN_TEST_CODE + i;
	if (!pass) {
		blog(LOG_ERROR, "bmem_overrun_test_check: failed at %p", ptr);
	}
}

#else // BMEM_TRACE
#define BMEM_TRACE_SIZE_BYTE 0
#define BMEM_OVERRUN_TEST_BYTE 0
#endif // BMEM_TRACE

static void *a_malloc(size_t size)
{
#ifdef ALIGNED_MALLOC
	return _aligned_malloc(size, ALIGNMENT);
#elif ALIGNMENT_HACK
	void *ptr = NULL;
	long diff;

	ptr = malloc(size + ALIGNMENT + BMEM_TRACE_SIZE_BYTE + BMEM_OVERRUN_TEST_BYTE);
	if (ptr) {
		diff = ((~(long)ptr) & (ALIGNMENT - 1)) + 1;
#ifdef BMEM_TRACE
		register_trace(ptr, size);
		diff += BMEM_TRACE_SIZE_BYTE;
#endif // BMEM_TRACE

		ptr = (char *)ptr + diff;
		((unsigned char *)ptr)[-1] = (unsigned char)diff;

		bmem_overrun_test_set(ptr + size);
	}

	return ptr;
#else
	return malloc(size);
#endif
}

static void *a_realloc(void *ptr, size_t size)
{
#ifdef ALIGNED_MALLOC
	return _aligned_realloc(ptr, size, ALIGNMENT);
#elif ALIGNMENT_HACK
	long diff;

	if (!ptr)
		return a_malloc(size);
	diff = ((unsigned char *)ptr)[-1];
#ifdef BMEM_TRACE
	bmem_overrun_test_check(ptr + ((struct bmem_trace *)((char *)ptr - diff))->size);
#endif
	ptr = realloc((char *)ptr - diff, size + diff + BMEM_OVERRUN_TEST_CODE);
	if (ptr) {
#ifdef BMEM_TRACE
		reregister_trace(ptr, size);
#endif
		ptr = (char *)ptr + diff;
#ifdef BMEM_TRACE
		bmem_overrun_test_set(ptr + size);
#endif
	}
	return ptr;
#else
	return realloc(ptr, size);
#endif
}

static void a_free(void *ptr)
{
#ifdef ALIGNED_MALLOC
	_aligned_free(ptr);
#elif ALIGNMENT_HACK
	if (ptr) {
		long diff = ((unsigned char *)ptr)[-1];
		ptr = (char *)ptr - diff;
#ifdef BMEM_TRACE
		bmem_overrun_test_check((char *)ptr + diff + ((struct bmem_trace *)ptr)->size);
		unregister_trace(ptr);
#endif // BMEM_TRACE
		free(ptr);
	}
#else
	free(ptr);
#endif
}

static long num_allocs = 0;

void *bmalloc(size_t size)
{
	if (!size) {
		blog(LOG_ERROR,
		     "bmalloc: Allocating 0 bytes is broken behavior, please "
		     "fix your code! This will crash in future versions of "
		     "OBS.");
		size = 1;
	}

	void *ptr = a_malloc(size);

	if (!ptr) {
		os_breakpoint();
		bcrash("Out of memory while trying to allocate %lu bytes",
		       (unsigned long)size);
	}

	os_atomic_inc_long(&num_allocs);
	return ptr;
}

void *brealloc(void *ptr, size_t size)
{
	if (!ptr)
		os_atomic_inc_long(&num_allocs);

	if (!size) {
		blog(LOG_ERROR,
		     "brealloc: Allocating 0 bytes is broken behavior, please "
		     "fix your code! This will crash in future versions of "
		     "OBS.");
		size = 1;
	}

	ptr = a_realloc(ptr, size);

	if (!ptr) {
		os_breakpoint();
		bcrash("Out of memory while trying to allocate %lu bytes",
		       (unsigned long)size);
	}

	return ptr;
}

void bfree(void *ptr)
{
	if (ptr) {
		os_atomic_dec_long(&num_allocs);
		a_free(ptr);
	}
}

long bnum_allocs(void)
{
	return num_allocs;
}

int base_get_alignment(void)
{
	return ALIGNMENT;
}

void *bmemdup(const void *ptr, size_t size)
{
	void *out = bmalloc(size);
	if (size)
		memcpy(out, ptr, size);

	return out;
}

OBS_DEPRECATED void base_set_allocator(struct base_allocator *defs)
{
	UNUSED_PARAMETER(defs);
}
