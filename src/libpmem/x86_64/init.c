/*
 * Copyright 2014-2018, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include <xmmintrin.h>
#include "libpmem.h"

#include "cpu.h"
#include "flush.h"
#include "memcpy_memset.h"
#include "os.h"
#include "out.h"
#include "pmem.h"
#include "valgrind_internal.h"

#define MOVNT_THRESHOLD	256

size_t Movnt_threshold = MOVNT_THRESHOLD;

/*
 * predrain_fence_empty -- (internal) issue the pre-drain fence instruction
 */
static void
predrain_fence_empty(void)
{
	LOG(15, NULL);

	VALGRIND_DO_FENCE;
	/* nothing to do (because CLFLUSH did it for us) */
}

/*
 * predrain_memory_barrier -- (internal) issue the pre-drain fence instruction
 */
static void
predrain_memory_barrier(void)
{
	LOG(15, NULL);
	_mm_sfence();	/* ensure CLWB or CLFLUSHOPT completes */
}

/*
 * flush_clflush -- (internal) flush the CPU cache, using clflush
 */
static void
flush_clflush(const void *addr, size_t len)
{
	LOG(15, "addr %p len %zu", addr, len);

	flush_clflush_nolog(addr, len);
}

/*
 * flush_clflushopt -- (internal) flush the CPU cache, using clflushopt
 */
static void
flush_clflushopt(const void *addr, size_t len)
{
	LOG(15, "addr %p len %zu", addr, len);

	flush_clflushopt_nolog(addr, len);
}

/*
 * flush_clwb -- (internal) flush the CPU cache, using clwb
 */
static void
flush_clwb(const void *addr, size_t len)
{
	LOG(15, "addr %p len %zu", addr, len);

	flush_clwb_nolog(addr, len);
}

/*
 * flush_empty -- (internal) do not flush the CPU cache
 */
static void
flush_empty(const void *addr, size_t len)
{
	LOG(15, "addr %p len %zu", addr, len);

	flush_empty_nolog(addr, len);
}

#if SSE2_AVAILABLE || AVX_AVAILABLE || AVX512F_AVAILABLE
#define MEMCPY_TEMPLATE(postfix) \
static void *\
memmove_nodrain_##postfix(void *dest, const void *src, size_t len)\
{\
	if (len == 0 || src == dest)\
		return dest;\
\
	if (len < Movnt_threshold)\
		memmove_mov_##postfix(dest, src, len);\
	else\
		memmove_movnt_##postfix(dest, src, len);\
\
	return dest;\
}

#define MEMSET_TEMPLATE(postfix)\
static void *\
memset_nodrain_##postfix(void *dest, int c, size_t len)\
{\
	if (len == 0)\
		return dest;\
\
	if (len < Movnt_threshold)\
		memset_mov_##postfix(dest, c, len);\
	else\
		memset_movnt_##postfix(dest, c, len);\
\
	return dest;\
}
#endif

#if SSE2_AVAILABLE
MEMCPY_TEMPLATE(sse2_clflush)
MEMCPY_TEMPLATE(sse2_clflushopt)
MEMCPY_TEMPLATE(sse2_clwb)
MEMCPY_TEMPLATE(sse2_empty)

MEMSET_TEMPLATE(sse2_clflush)
MEMSET_TEMPLATE(sse2_clflushopt)
MEMSET_TEMPLATE(sse2_clwb)
MEMSET_TEMPLATE(sse2_empty)
#endif

#if AVX_AVAILABLE
MEMCPY_TEMPLATE(avx_clflush)
MEMCPY_TEMPLATE(avx_clflushopt)
MEMCPY_TEMPLATE(avx_clwb)
MEMCPY_TEMPLATE(avx_empty)

MEMSET_TEMPLATE(avx_clflush)
MEMSET_TEMPLATE(avx_clflushopt)
MEMSET_TEMPLATE(avx_clwb)
MEMSET_TEMPLATE(avx_empty)
#endif

#if AVX512F_AVAILABLE
MEMCPY_TEMPLATE(avx512f_clflush)
MEMCPY_TEMPLATE(avx512f_clflushopt)
MEMCPY_TEMPLATE(avx512f_clwb)
MEMCPY_TEMPLATE(avx512f_empty)

MEMSET_TEMPLATE(avx512f_clflush)
MEMSET_TEMPLATE(avx512f_clflushopt)
MEMSET_TEMPLATE(avx512f_clwb)
MEMSET_TEMPLATE(avx512f_empty)
#endif

/*
 * memmove_nodrain_libc -- (internal) memmove to pmem without hw drain
 */
static void *
memmove_nodrain_libc(void *pmemdest, const void *src, size_t len)
{
	LOG(15, "pmemdest %p src %p len %zu", pmemdest, src, len);

	memmove(pmemdest, src, len);
	pmem_flush(pmemdest, len);
	return pmemdest;
}

/*
 * memset_nodrain_libc -- (internal) memset to pmem without hw drain, normal
 */
static void *
memset_nodrain_libc(void *pmemdest, int c, size_t len)
{
	LOG(15, "pmemdest %p c 0x%x len %zu", pmemdest, c, len);

	memset(pmemdest, c, len);
	pmem_flush(pmemdest, len);
	return pmemdest;
}

static void
use_sse2_memcpy_memset(struct pmem_funcs *funcs)
{
#if SSE2_AVAILABLE
	if (funcs->deep_flush == flush_clflush)
		funcs->memmove_nodrain = memmove_nodrain_sse2_clflush;
	else if (funcs->deep_flush == flush_clflushopt)
		funcs->memmove_nodrain = memmove_nodrain_sse2_clflushopt;
	else if (funcs->deep_flush == flush_clwb)
		funcs->memmove_nodrain = memmove_nodrain_sse2_clwb;
	else if (funcs->deep_flush == flush_empty)
		funcs->memmove_nodrain = memmove_nodrain_sse2_empty;
	else
		ASSERT(0);

	if (funcs->deep_flush == flush_clflush)
		funcs->memset_nodrain = memset_nodrain_sse2_clflush;
	else if (funcs->deep_flush == flush_clflushopt)
		funcs->memset_nodrain = memset_nodrain_sse2_clflushopt;
	else if (funcs->deep_flush == flush_clwb)
		funcs->memset_nodrain = memset_nodrain_sse2_clwb;
	else if (funcs->deep_flush == flush_empty)
		funcs->memset_nodrain = memset_nodrain_sse2_empty;
	else
		ASSERT(0);
#else
	LOG(3, "sse2 disabled at build time");
#endif

}

static void
use_avx_memcpy_memset(struct pmem_funcs *funcs)
{
#if AVX_AVAILABLE
	LOG(3, "avx supported");

	char *e = os_getenv("PMEM_AVX");
	if (e == NULL || strcmp(e, "1") != 0) {
		LOG(3, "PMEM_AVX not set or not == 1");
		return;
	}

	LOG(3, "PMEM_AVX enabled");

	if (funcs->deep_flush == flush_clflush)
		funcs->memmove_nodrain = memmove_nodrain_avx_clflush;
	else if (funcs->deep_flush == flush_clflushopt)
		funcs->memmove_nodrain = memmove_nodrain_avx_clflushopt;
	else if (funcs->deep_flush == flush_clwb)
		funcs->memmove_nodrain = memmove_nodrain_avx_clwb;
	else if (funcs->deep_flush == flush_empty)
		funcs->memmove_nodrain = memmove_nodrain_avx_empty;
	else
		ASSERT(0);

	if (funcs->deep_flush == flush_clflush)
		funcs->memset_nodrain = memset_nodrain_avx_clflush;
	else if (funcs->deep_flush == flush_clflushopt)
		funcs->memset_nodrain = memset_nodrain_avx_clflushopt;
	else if (funcs->deep_flush == flush_clwb)
		funcs->memset_nodrain = memset_nodrain_avx_clwb;
	else if (funcs->deep_flush == flush_empty)
		funcs->memset_nodrain = memset_nodrain_avx_empty;
	else
		ASSERT(0);
#else
	LOG(3, "avx supported, but disabled at build time");
#endif
}

static void
use_avx512f_memcpy_memset(struct pmem_funcs *funcs)
{
#if AVX512F_AVAILABLE
	LOG(3, "avx512f supported");

	char *e = os_getenv("PMEM_AVX512F");
	if (e == NULL || strcmp(e, "1") != 0) {
		LOG(3, "PMEM_AVX512F not set or not == 1");
		return;
	}

	LOG(3, "PMEM_AVX512F enabled");

	if (funcs->deep_flush == flush_clflush)
		funcs->memmove_nodrain = memmove_nodrain_avx512f_clflush;
	else if (funcs->deep_flush == flush_clflushopt)
		funcs->memmove_nodrain = memmove_nodrain_avx512f_clflushopt;
	else if (funcs->deep_flush == flush_clwb)
		funcs->memmove_nodrain = memmove_nodrain_avx512f_clwb;
	else if (funcs->deep_flush == flush_empty)
		funcs->memmove_nodrain = memmove_nodrain_avx512f_empty;
	else
		ASSERT(0);

	if (funcs->deep_flush == flush_clflush)
		funcs->memset_nodrain = memset_nodrain_avx512f_clflush;
	else if (funcs->deep_flush == flush_clflushopt)
		funcs->memset_nodrain = memset_nodrain_avx512f_clflushopt;
	else if (funcs->deep_flush == flush_clwb)
		funcs->memset_nodrain = memset_nodrain_avx512f_clwb;
	else if (funcs->deep_flush == flush_empty)
		funcs->memset_nodrain = memset_nodrain_avx512f_empty;
	else
		ASSERT(0);
#else
	LOG(3, "avx512f supported, but disabled at build time");
#endif
}

/*
 * pmem_get_cpuinfo -- configure libpmem based on CPUID
 */
static void
pmem_cpuinfo_to_funcs(struct pmem_funcs *funcs)
{
	LOG(3, NULL);

	if (is_cpu_clflush_present()) {
		funcs->is_pmem = is_pmem_detect;
		LOG(3, "clflush supported");
	}

	if (is_cpu_clflushopt_present()) {
		LOG(3, "clflushopt supported");

		char *e = os_getenv("PMEM_NO_CLFLUSHOPT");
		if (e && strcmp(e, "1") == 0)
			LOG(3, "PMEM_NO_CLFLUSHOPT forced no clflushopt");
		else {
			funcs->deep_flush = flush_clflushopt;
			funcs->predrain_fence = predrain_memory_barrier;
		}
	}

	if (is_cpu_clwb_present()) {
		LOG(3, "clwb supported");

		char *e = os_getenv("PMEM_NO_CLWB");
		if (e && strcmp(e, "1") == 0)
			LOG(3, "PMEM_NO_CLWB forced no clwb");
		else {
			funcs->deep_flush = flush_clwb;
			funcs->predrain_fence = predrain_memory_barrier;
		}
	}

	char *ptr = os_getenv("PMEM_NO_MOVNT");
	if (ptr && strcmp(ptr, "1") == 0) {
		LOG(3, "PMEM_NO_MOVNT forced no movnt");
	} else {
		use_sse2_memcpy_memset(funcs);

		if (is_cpu_avx_present())
			use_avx_memcpy_memset(funcs);

		if (is_cpu_avx512f_present())
			use_avx512f_memcpy_memset(funcs);
	}
}

struct pmem_funcs
pmem_arch_init(void)
{
	struct pmem_funcs funcs;

	funcs.predrain_fence = predrain_fence_empty;
	funcs.deep_flush = flush_clflush;
	funcs.is_pmem = NULL;
	funcs.memmove_nodrain = memmove_nodrain_libc;
	funcs.memset_nodrain = memset_nodrain_libc;

	pmem_cpuinfo_to_funcs(&funcs);

	funcs.flush = funcs.deep_flush;

	/*
	 * For testing, allow overriding the default threshold
	 * for using non-temporal stores in pmem_memcpy_*(), pmem_memmove_*()
	 * and pmem_memset_*().
	 * It has no effect if movnt is not supported or disabled.
	 */
	char *ptr = os_getenv("PMEM_MOVNT_THRESHOLD");
	if (ptr) {
		long long val = atoll(ptr);

		if (val < 0)
			LOG(3, "Invalid PMEM_MOVNT_THRESHOLD");
		else {
			LOG(3, "PMEM_MOVNT_THRESHOLD set to %zu", (size_t)val);
			Movnt_threshold = (size_t)val;
		}
	}

	char *e = os_getenv("PMEM_NO_FLUSH");
	if (e && strcmp(e, "1") == 0) {
		LOG(3, "forced not flushing CPU cache");
		funcs.flush = flush_empty;
		funcs.predrain_fence = predrain_memory_barrier;
	}

	if (funcs.deep_flush == flush_clwb)
		LOG(3, "using clwb");
	else if (funcs.deep_flush == flush_clflushopt)
		LOG(3, "using clflushopt");
	else if (funcs.deep_flush == flush_clflush)
		LOG(3, "using clflush");
	else
		FATAL("invalid deep flush function address");

	if (funcs.flush == flush_empty)
		LOG(3, "not flushing CPU cache");
	else if (funcs.flush != funcs.deep_flush)
		FATAL("invalid flush function address");

#if AVX512F_AVAILABLE
	if (funcs.memmove_nodrain == memmove_nodrain_avx512f_clflush ||
		funcs.memmove_nodrain == memmove_nodrain_avx512f_clflushopt ||
		funcs.memmove_nodrain == memmove_nodrain_avx512f_clwb ||
		funcs.memmove_nodrain == memmove_nodrain_avx512f_empty)
		LOG(3, "using movnt AVX512F");
	else
#endif
#if AVX_AVAILABLE
	if (funcs.memmove_nodrain == memmove_nodrain_avx_clflush ||
		funcs.memmove_nodrain == memmove_nodrain_avx_clflushopt ||
		funcs.memmove_nodrain == memmove_nodrain_avx_clwb ||
		funcs.memmove_nodrain == memmove_nodrain_avx_empty)
		LOG(3, "using movnt AVX");
	else
#endif
#if SSE2_AVAILABLE
	if (funcs.memmove_nodrain == memmove_nodrain_sse2_clflush ||
		funcs.memmove_nodrain == memmove_nodrain_sse2_clflushopt ||
		funcs.memmove_nodrain == memmove_nodrain_sse2_clwb ||
		funcs.memmove_nodrain == memmove_nodrain_sse2_empty)
		LOG(3, "using movnt SSE2");
	else
#endif
	if (funcs.memmove_nodrain == memmove_nodrain_libc)
		LOG(3, "using libc memmove");
	else
		FATAL("invalid memove_nodrain function address");

	return funcs;
}
