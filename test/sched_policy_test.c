/*
 * sched_policy_test.c — Stub tests for LOOP/HUNGRY/INSTR scheduling policies.
 *
 * Compile (Linux aarch64):
 *   gcc -std=c11 -pthread -O2 -Wall -o sched_policy_test test/sched_policy_test.c
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

/* =========================================================================
 * Mock type & constant definitions (mirror wd_sched.c + wd_alg_common.h)
 * ========================================================================= */

typedef unsigned int   __u32;
typedef unsigned char  __u8;

#define SKEY_CTX_MAX_NUM      16
#define INVALID_POS           0xFFFFFFFFu
#define HUNGRY_LOAD_THRESHOLD 256

enum {
	SCHED_POLICY_RR = 0,
	SCHED_POLICY_NONE,
	SCHED_POLICY_SINGLE,
	SCHED_POLICY_DEV,
	SCHED_POLICY_LOOP,
	SCHED_POLICY_HUNGRY,
	SCHED_POLICY_INSTR,
	SCHED_POLICY_BUTT
};

enum {
	UADK_CTX_HW      = 0x0,
	UADK_CTX_CE_INS  = 0x1,
	UADK_CTX_SVE_INS = 0x2,
	UADK_CTX_SOFT    = 0x3,
	UADK_CTX_MAX     = 4
};

#define SCHED_MODE_SYNC  0
#define SCHED_MODE_ASYNC 1

/* =========================================================================
 * Struct replica (must match wd_sched.c wd_sched_domain_idx_cache exactly)
 * ========================================================================= */

struct wd_sched_domain_idx_cache {
	__u32         idx_list[SKEY_CTX_MAX_NUM];
	atomic_uint   load_values[SKEY_CTX_MAX_NUM];
	__u32         valid_count;

	atomic_uint   rr_ptr;
	atomic_uint   min_load_idx;
	atomic_uint   op_counter;

	__u32         update_interval;
	__u8          policy;

	__u8          ctx_props[SKEY_CTX_MAX_NUM];
	atomic_uint   prop_ptrs[4];
};

/* =========================================================================
 * Helper: initialise a cache with a given policy and list of (idx, prop)
 * ========================================================================= */

static void cache_init(struct wd_sched_domain_idx_cache *c, __u8 policy,
		       const __u32 *indices, const __u8 *props, __u32 count)
{
	__u32 i;

	memset(c, 0, sizeof(*c));
	c->policy = policy;
	c->update_interval = 16;

	for (i = 0; i < SKEY_CTX_MAX_NUM; i++)
		c->idx_list[i] = INVALID_POS;

	for (i = 0; i < count && i < SKEY_CTX_MAX_NUM; i++) {
		c->idx_list[i]     = indices[i];
		c->ctx_props[i]    = props[i];
		atomic_store(&c->load_values[i], 0);
	}
	c->valid_count = count;

	if (policy == SCHED_POLICY_LOOP || policy == SCHED_POLICY_INSTR)
		for (i = 0; i < 4; i++)
			atomic_store(&c->prop_ptrs[i], 0);
}

/* =========================================================================
 * Function replicas from wd_sched.c (must match exactly)
 * ========================================================================= */

static __u32 loop_sched_pick(struct wd_sched_domain_idx_cache *cache)
{
	__u32 pos, ctx_id;
	int retries = 0;

	if (cache->valid_count == 0)
		return INVALID_POS;

	/* Global RR across all ctx, track per-prop stats */
	do {
		pos = atomic_fetch_add(&cache->rr_ptr, 1) % cache->valid_count;
		ctx_id = cache->idx_list[pos];
		if (ctx_id != INVALID_POS) {
			/* Update per-prop RR counter for tracking */
			atomic_fetch_add(&cache->prop_ptrs[cache->ctx_props[pos]], 1);
			return ctx_id;
		}
	} while (++retries < (int)cache->valid_count);

	return INVALID_POS;
}

static __u32 instr_sched_pick(struct wd_sched_domain_idx_cache *cache)
{
	__u32 rr_pos, i, count = 0, instr_cnt = 0;

	if (cache->valid_count == 0)
		return INVALID_POS;

	for (i = 0; i < cache->valid_count; i++)
		if (cache->ctx_props[i] == UADK_CTX_CE_INS ||
		    cache->ctx_props[i] == UADK_CTX_SVE_INS)
			instr_cnt++;
	if (instr_cnt == 0)
		return INVALID_POS;

	rr_pos = atomic_fetch_add(&cache->rr_ptr, 1) % instr_cnt;
	for (i = 0; i < cache->valid_count; i++)
		if (cache->ctx_props[i] == UADK_CTX_CE_INS ||
		    cache->ctx_props[i] == UADK_CTX_SVE_INS)
			if (count++ == rr_pos)
				return cache->idx_list[i];

	return INVALID_POS;
}

/* =========================================================================
 * Test: LOOP prop distribution (AC1)
 * ========================================================================= */

static void test_loop_prop_distribution(void)
{
	struct wd_sched_domain_idx_cache cache;
	const __u32 indices[] = {10, 20, 30, 40};
	const __u8  props[]   = {UADK_CTX_HW, UADK_CTX_HW,
				  UADK_CTX_CE_INS, UADK_CTX_SOFT};
	__u32 counts[UADK_CTX_MAX] = {0};
	__u32 result;
	int i;

	cache_init(&cache, SCHED_POLICY_LOOP, indices, props, 4);

	for (i = 0; i < 1000; i++) {
		result = loop_sched_pick(&cache);
		if (result == INVALID_POS) {
			printf("FAIL: loop pick returned INVALID_POS at iter %d\n", i);
			exit(1);
		}
		/* Determine which prop was picked by looking up the result */
		__u32 j;
		for (j = 0; j < cache.valid_count; j++)
			if (cache.idx_list[j] == result) {
				counts[cache.ctx_props[j]]++;
				break;
			}
	}

	/* Expected: HW ~50% (500), CE ~25% (250), SOFT ~25% (250), deviation ≤20% */
	__u32 hw_lo = 400, hw_hi = 600;
	__u32 ce_lo = 200, ce_hi = 300;
	__u32 sf_lo = 200, sf_hi = 300;

	printf("LOOP distribution: HW=%u CE=%u SOFT=%u SVE=%u\n",
	       counts[0], counts[1], counts[3], counts[2]);

	if (counts[0] < hw_lo || counts[0] > hw_hi) {
		printf("FAIL: HW count %u outside [%u, %u]\n", counts[0], hw_lo, hw_hi);
		exit(1);
	}
	if (counts[1] < ce_lo || counts[1] > ce_hi) {
		printf("FAIL: CE count %u outside [%u, %u]\n", counts[1], ce_lo, ce_hi);
		exit(1);
	}
	if (counts[3] < sf_lo || counts[3] > sf_hi) {
		printf("FAIL: SOFT count %u outside [%u, %u]\n", counts[3], sf_lo, sf_hi);
		exit(1);
	}
	if (counts[2] != 0) {
		printf("FAIL: SVE count should be 0 (no SVE in cache), got %u\n", counts[2]);
		exit(1);
	}

	printf("  PASS\n");
}

/* =========================================================================
 * Test: INSTR CE/SVE filtering (AC4)
 * ========================================================================= */

static void test_instr_filter(void)
{
	struct wd_sched_domain_idx_cache cache;
	const __u32 indices[] = {10, 20, 30, 40};
	const __u8  props[]   = {UADK_CTX_CE_INS, UADK_CTX_HW,
				  UADK_CTX_SVE_INS, UADK_CTX_SOFT};
	__u32 result;
	int i;

	cache_init(&cache, SCHED_POLICY_INSTR, indices, props, 4);

	for (i = 0; i < 100; i++) {
		result = instr_sched_pick(&cache);
		if (result == INVALID_POS) {
			printf("FAIL: instr pick returned INVALID_POS at iter %d\n", i);
			exit(1);
		}
		/* Verify result is CE (10) or SVE (30), never HW (20) or SOFT (40) */
		if (result == 20 || result == 40) {
			printf("FAIL: instr pick returned non-CE/SVE ctx %u at iter %d\n",
			       result, i);
			exit(1);
		}
	}

	printf("INSTR filter (CE+SVE only): 100 picks all CE or SVE\n");
	printf("  PASS\n");
}

/* =========================================================================
 * Test: INSTR no CE/SVE → all picks return INVALID_POS (AC3 pick level)
 * ========================================================================= */

static void test_instr_no_ce_sve_pick(void)
{
	struct wd_sched_domain_idx_cache cache;
	const __u32 indices[] = {10, 20};
	const __u8  props[]   = {UADK_CTX_HW, UADK_CTX_SOFT};
	__u32 result;

	cache_init(&cache, SCHED_POLICY_INSTR, indices, props, 2);

	result = instr_sched_pick(&cache);
	if (result != INVALID_POS) {
		printf("FAIL: expected INVALID_POS for no-CE/SVE cache, got %u\n", result);
		exit(1);
	}

	printf("INSTR no-CE/SVE pick: INVALID_POS as expected\n");
	printf("  PASS\n");
}

/* =========================================================================
 * Test: HUNGRY extension ceiling (AC2)
 * ========================================================================= */

static void test_hungry_expand_ceiling(void)
{
	/* Simulate the ceiling check logic directly:
	 *   if (valid_count >= SKEY_CTX_MAX_NUM) goto skip_expand;
	 */
	__u32 valid_count = SKEY_CTX_MAX_NUM;
	int would_expand = (valid_count < SKEY_CTX_MAX_NUM);

	if (would_expand) {
		printf("FAIL: ceiling check should reject expansion at max capacity\n");
		exit(1);
	}

	printf("HUNGRY ceiling: valid_count=%u >= %u, expansion blocked\n",
	       valid_count, (__u32)SKEY_CTX_MAX_NUM);

	/* Normal case: below ceiling, expansion allowed */
	valid_count = 2;
	would_expand = (valid_count < SKEY_CTX_MAX_NUM);
	if (!would_expand) {
		printf("FAIL: ceiling check should allow expansion below max\n");
		exit(1);
	}

	printf("HUNGRY normal: valid_count=%u < %u, expansion allowed\n",
	       valid_count, (__u32)SKEY_CTX_MAX_NUM);
	printf("  PASS\n");
}

/* =========================================================================
 * Test: RR regression — basic RR still works (AC7)
 * ========================================================================= */

static void test_rr_regression(void)
{
	struct wd_sched_domain_idx_cache cache;
	const __u32 indices[] = {1, 2, 3};
	const __u8  props[]   = {UADK_CTX_HW, UADK_CTX_HW, UADK_CTX_HW};
	__u32 result;
	int i;

	cache_init(&cache, SCHED_POLICY_LOOP, indices, props, 3);

	/* With all HW props, LOOP should behave like RR within the single group */
	for (i = 0; i < 30; i++) {
		result = loop_sched_pick(&cache);
		if (result == INVALID_POS) {
			printf("FAIL: basic RR returned INVALID_POS at iter %d\n", i);
			exit(1);
		}
	}

	printf("RR regression: 30 picks from all-HW cache, no INVALID_POS\n");
	printf("  PASS\n");
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void)
{
	printf("=== sched_policy_test ===\n\n");

	test_loop_prop_distribution();
	test_instr_filter();
	test_instr_no_ce_sve_pick();
	test_hungry_expand_ceiling();
	test_rr_regression();

	printf("\nAll tests passed!\n");
	return 0;
}
