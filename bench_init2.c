/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <numa.h>
#include <sys/mman.h>
#include <sys/time.h>

#include "wd_cipher.h"
#include "wd_sched.h"
#include "wd_alg_common.h"

#define PKT_LEN    4096
#define IV_SIZE    16
#define KEY_SIZE   16
#define CTX_NUMS   16
#define BENCH_SECS 3

static volatile int g_running = 1;
static volatile __u32 g_completed = 0;

static void *cipher_cb(struct wd_cipher_req *req, void *cb_param)
{
	__sync_fetch_and_add(&g_completed, 1);
	return NULL;
}

static int run_benchmark(int sched_type, int task_type, const char *name)
{
	struct wd_ctx_params ctx_params = {0};
	struct wd_ctx_nums ctx_nums[2] = {0};
	struct wd_cap_config cap = {0};
	struct wd_cipher_sess_setup setup = {0};
	struct wd_cipher_req req = {0};
	struct sched_params sp = {0};
	struct timeval start, end;
	handle_t h_sess;
	__u8 *src, *dst;
	__u8 iv[IV_SIZE];
	__u8 key[KEY_SIZE];
	__u64 total_sent = 0;
	double elapsed, kops;
	int ret, try_cnt;

	fprintf(stderr, "\n=== Benchmark: %-12s  TASK_%s  (CTX=%d) ===\n",
	       name, task_type == TASK_HW ? "HW" : "MIX", CTX_NUMS);

	/* Buffers */
	src = mmap(NULL, PKT_LEN, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	dst = mmap(NULL, PKT_LEN, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (src == MAP_FAILED || dst == MAP_FAILED) {
		printf("FAIL: mmap\n");
		return -1;
	}
	memset(src, 0xCC, PKT_LEN);
	memset(dst, 0, PKT_LEN);
	memset(key, 0xAA, KEY_SIZE);
	memset(iv, 0xBB, IV_SIZE);

	/* Init2 */
	cap.ctx_msg_num = 2048;
	ctx_nums[0].sync_ctx_num = CTX_NUMS;
	ctx_nums[0].async_ctx_num = CTX_NUMS;
	ctx_nums[1].sync_ctx_num = CTX_NUMS;
	ctx_nums[1].async_ctx_num = CTX_NUMS;
	ctx_params.op_type_num = 2;
	ctx_params.bmp = numa_allocate_nodemask();
	numa_bitmask_setbit(ctx_params.bmp, 0);
	numa_bitmask_setbit(ctx_params.bmp, 1);
	ctx_params.cap = &cap;
	ctx_params.ctx_set_num = ctx_nums;

	ret = wd_cipher_init2_("ctr(sm4)", sched_type, task_type, &ctx_params);
	if (ret) { fprintf(stderr, "FAIL: init2 ret=%d\n", ret); goto out; }

	/* Session */
	setup.alg = WD_CIPHER_SM4;
	setup.mode = WD_CIPHER_CTR;
	setup.mm_type = UADK_MEM_AUTO;
	sp.numa_id = 0;
	sp.type = 0;
	sp.mode = 0;
	setup.sched_param = &sp;

	h_sess = wd_cipher_alloc_sess(&setup);
	if (!h_sess) { fprintf(stderr, "FAIL: alloc_sess\n"); goto out_uninit; }

	ret = wd_cipher_set_key(h_sess, key, KEY_SIZE);
	if (ret) { fprintf(stderr, "FAIL: set_key ret=%d\n", ret); goto out_free; }

	/* Benchmark loop */
	req.src = src;
	req.dst = dst;
	req.iv = iv;
	req.iv_bytes = IV_SIZE;
	req.in_bytes = PKT_LEN;
	req.out_bytes = PKT_LEN;
	req.out_buf_bytes = PKT_LEN;
	req.op_type = WD_CIPHER_ENCRYPTION;
	req.data_fmt = 0;
	req.state = 0;
	req.cb = cipher_cb;

	g_running = 1;
	g_completed = 0;
	total_sent = 0;

	gettimeofday(&start, NULL);

	while (g_running) {
		try_cnt = 0;
		ret = wd_do_cipher_async(h_sess, &req);
		if (ret >= 0) {
			total_sent++;
			/* Poll every 64 submissions */
			if ((total_sent & 63) == 0)
				do { __u32 _c; wd_cipher_poll(32, &_c); } while(0);
			/* Check time every 1024 ops */
			if ((total_sent & 1023) == 0) {
				gettimeofday(&end, NULL);
				if (end.tv_sec - start.tv_sec >= BENCH_SECS)
					g_running = 0;
			}
			continue;
		}
		/* Queue full - poll and retry */
		do { __u32 _c; wd_cipher_poll(16, &_c); } while(0);
		if (++try_cnt > 1000) {
			usleep(10);
			try_cnt = 0;
		}
		/* Check time */
		gettimeofday(&end, NULL);
		if (end.tv_sec - start.tv_sec >= BENCH_SECS)
			g_running = 0;
	}

	gettimeofday(&end, NULL);

	/* Drain remaining (with timeout) */
	{
		int drain_timeout = 5000; /* 5 seconds max */
		while (g_completed < total_sent && drain_timeout-- > 0) {
			usleep(1000);
			do { __u32 _c; wd_cipher_poll(64, &_c); } while(0);
		}
	}

	elapsed = (end.tv_sec - start.tv_sec) +
		  (end.tv_usec - start.tv_usec) / 1000000.0;
	kops = total_sent / 1000.0 / elapsed;

	fprintf(stderr, "  Sent: %llu  Completed: %u  Time: %.2fs  Kops: %.1f\n",
	       (unsigned long long)total_sent, g_completed, elapsed, kops);

out_free:
	wd_cipher_free_sess(h_sess);
out_uninit:
	wd_cipher_uninit();
out:
	numa_free_nodemask(ctx_params.bmp);
	munmap(src, PKT_LEN);
	munmap(dst, PKT_LEN);
	return ret;
}

int main(int argc, char **argv)
{
	int sched = SCHED_POLICY_RR;
	int task_type = TASK_HW;
	const char *name = "RR";
	const char *task_name = "HW";

	if (argc > 1) {
		if (!strcmp(argv[1], "hungry")) { sched = SCHED_POLICY_HUNGRY; name = "HUNGRY"; }
		else if (!strcmp(argv[1], "loop")) { sched = SCHED_POLICY_LOOP; name = "LOOP"; }
		else if (!strcmp(argv[1], "rr")) { sched = SCHED_POLICY_RR; name = "RR"; }
	}
	if (argc > 2) {
		if (!strcmp(argv[2], "mix")) { task_type = TASK_MIX; task_name = "MIX"; }
	}
	setbuf(stderr, NULL);
	fprintf(stderr, "=== Testing %s/%s ===\n", name, task_name);
	run_benchmark(sched, task_type, name);
	return 0;
}
