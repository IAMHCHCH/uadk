/* SPDX-License-Identifier: Apache-2.0 */
/*
 * bench_sched.c — Multi-dimensional UADK cipher scheduling benchmark
 *
 * Covers: SYNC/ASYNC × init/init2 × RR/LOOP/HUNGRY/INSTR × HW/MIX × 1..N threads
 *
 * Architecture:
 *   global_init()  — wd_cipher_init2_() or wd_cipher_init()  (once)
 *   session_init() — alloc_sess + set_key                     (per thread)
 *   run_bench()    — benchmark loop                           (per thread)
 *   session_cleanup() — free_sess                             (per thread)
 *   global_uninit()   — wd_cipher_uninit2() or wd_cipher_uninit() (once)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <numa.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <errno.h>

#include "wd_cipher.h"
#include "wd_sched.h"
#include "wd_alg_common.h"

/* ── defaults ─────────────────────────────────────────────────────── */
#define PKT_LEN         4096
#define IV_SIZE         16
#define KEY_SIZE        16
#define CTX_NUMS        16
#define BENCH_SECS      3
#define MAX_THREADS     64
#define LAT_SAMPLE_MAX  100000
#define LAT_RING_SIZE   4096

/* ── benchmark configuration ──────────────────────────────────────── */
enum bench_mode   { BENCH_ASYNC, BENCH_SYNC };
enum bench_init   { BENCH_INIT_OLD, BENCH_INIT2 };

struct bench_config {
	enum bench_mode  mode;
	enum bench_init  init_type;
	int              sched_policy;
	int              task_type;
	int              nthreads;
	int              duration_secs;
	int              pkt_size;
	int              ctx_nums;
	const char      *alg_name;
	int              ctx_msg_num;
	bool             show_prop_stats;
};

/* ── per-thread stats ─────────────────────────────────────────────── */
struct thread_stats {
	__u64  total_sent;
	__u64  total_completed;
	double elapsed_sec;
	__u64  lat_samples[LAT_SAMPLE_MAX];
	int    lat_count;
};

/* ── helpers ──────────────────────────────────────────────────────── */
static __u64 now_us(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (__u64)tv.tv_sec * 1000000ULL + (__u64)tv.tv_usec;
}

static double elapsed_sec_since(struct timeval *start)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	return (now.tv_sec - start->tv_sec)
	     + (now.tv_usec - start->tv_usec) / 1000000.0;
}

static int cmp_u64(const void *a, const void *b)
{
	__u64 va = *(const __u64 *)a, vb = *(const __u64 *)b;
	return (va > vb) - (va < vb);
}

static void print_latency_stats(struct thread_stats *st)
{
	if (st->lat_count < 2)
		return;

	qsort(st->lat_samples, st->lat_count, sizeof(__u64), cmp_u64);

	double avg = 0;
	for (int i = 0; i < st->lat_count; i++)
		avg += (double)st->lat_samples[i];
	avg /= (double)st->lat_count;

	int p50_idx = (int)(st->lat_count * 0.50);
	int p99_idx = (int)(st->lat_count * 0.99);

	fprintf(stderr, "  Latency: avg=%.1fus  P50=%.1fus  P99=%.1fus  (samples=%d)\n",
		avg, (double)st->lat_samples[p50_idx],
		(double)st->lat_samples[p99_idx], st->lat_count);
}

/* ── async context ───────────────────────────────────────────────── */
struct async_ctx {
	handle_t              h_sess;
	struct wd_cipher_req  req;
	__u8                 *src, *dst;
	__u8                  iv[IV_SIZE];
	__u8                  key[KEY_SIZE];
	volatile int          running;
	volatile __u32        completed;
	/* latency tracking */
	__u64                 send_times[LAT_RING_SIZE];
	struct thread_stats  *st;
};

static void *async_cb(struct wd_cipher_req *req, void *cb_param)
{
	struct async_ctx *ctx = (struct async_ctx *)cb_param;
	__u32 idx = __sync_fetch_and_add(&ctx->completed, 1);
	__u64 now = now_us();
	__u64 lat = now - ctx->send_times[idx & (LAT_RING_SIZE - 1)];

	if (ctx->st && ctx->st->lat_count < LAT_SAMPLE_MAX)
		ctx->st->lat_samples[ctx->st->lat_count++] = lat;

	return NULL;
}

/* ── forward decls ─────────────────────────────────────────────────── */
static int  global_init(struct bench_config *cfg);
static void global_uninit(struct bench_config *cfg);
static int  session_init(struct bench_config *cfg, struct async_ctx *ctx);
static void session_cleanup(struct async_ctx *ctx, struct bench_config *cfg);

/* ── global init ──────────────────────────────────────────────────── */

/* Global state for init2 (shared across threads) */
static struct {
	struct bitmask *bmp;    /* saved for uninit cleanup */
	int             ready;
} g_init2;

/* Global state for old init (shared across threads) */
#define OLD_MAX_CTX 256
static struct {
	struct wd_ctx_config  ctx_cfg;
	struct wd_sched      *sched;
	int                   nctxs;
	int                   ready;
} g_old;

static int global_init(struct bench_config *cfg)
{
	if (cfg->init_type == BENCH_INIT_OLD) {
		int max_node = numa_max_node() + 1;
		int nctxs = cfg->ctx_nums * 2;
		int ret;

		memset(&g_old, 0, sizeof(g_old));
		g_old.nctxs = nctxs;

		g_old.ctx_cfg.ctx_num = nctxs;
		g_old.ctx_cfg.ctxs = calloc(nctxs, sizeof(struct wd_ctx));
		if (!g_old.ctx_cfg.ctxs)
			return -ENOMEM;

		for (int i = 0; i < nctxs; i++) {
			struct uacce_dev *dev = wd_get_accel_dev("cipher");
			if (!dev) {
				fprintf(stderr, "FAIL: old_init: wd_get_accel_dev failed\n");
				ret = -ENODEV; goto free_ctxs;
			}
			if (dev->numa_id < 0) dev->numa_id = 0;
			g_old.ctx_cfg.ctxs[i].ctx = wd_request_ctx(dev);
			if (!g_old.ctx_cfg.ctxs[i].ctx) {
				fprintf(stderr, "FAIL: old_init: wd_request_ctx[%d] failed\n", i);
				free(dev);
				ret = -ENODEV; goto release_ctxs;
			}
			g_old.ctx_cfg.ctxs[i].op_type = (i < nctxs / 2) ? 0 : 1;
			g_old.ctx_cfg.ctxs[i].ctx_mode = (cfg->mode == BENCH_SYNC)
				? CTX_MODE_SYNC : CTX_MODE_ASYNC;
			free(dev);
		}

		g_old.sched = wd_sched_rr_alloc(cfg->sched_policy, 2, max_node,
						wd_cipher_poll_ctx);
		if (!g_old.sched) {
			fprintf(stderr, "FAIL: old_init: wd_sched_rr_alloc failed\n");
			ret = -1; goto release_ctxs;
		}

		struct sched_params param = {0};
		param.numa_id = 0; param.type = 0; param.mode = 0;
		param.begin = 0; param.end = nctxs - 1;
		ret = wd_sched_rr_instance(g_old.sched, &param);
		if (ret) {
			fprintf(stderr, "FAIL: old_init: wd_sched_rr_instance ret=%d\n", ret);
			goto release_sched;
		}

		ret = wd_cipher_init(&g_old.ctx_cfg, g_old.sched);
		if (ret) {
			fprintf(stderr, "FAIL: old_init: wd_cipher_init ret=%d\n", ret);
			goto release_sched;
		}

		g_old.ready = 1;
		return 0;

	release_sched:
		wd_sched_rr_release(g_old.sched);
		g_old.sched = NULL;
	release_ctxs:
		for (int i = 0; i < nctxs; i++) {
			if (g_old.ctx_cfg.ctxs[i].ctx)
				wd_release_ctx(g_old.ctx_cfg.ctxs[i].ctx);
		}
	free_ctxs:
		free(g_old.ctx_cfg.ctxs);
		g_old.ctx_cfg.ctxs = NULL;
		return ret;
	}

	/* ── init2 global init ──────────────────────────────────────── */
	struct wd_ctx_params  ctx_params = {0};
	struct wd_ctx_nums    ctx_nums[2] = {0};
	struct wd_cap_config  cap         = {0};
	int ret;

	cap.ctx_msg_num = cfg->ctx_msg_num;
	ctx_nums[0].sync_ctx_num  = cfg->ctx_nums;
	ctx_nums[0].async_ctx_num = cfg->ctx_nums;
	ctx_nums[1].sync_ctx_num  = cfg->ctx_nums;
	ctx_nums[1].async_ctx_num = cfg->ctx_nums;

	ctx_params.op_type_num = 2;
	ctx_params.bmp = numa_allocate_nodemask();
	if (!ctx_params.bmp) return -ENOMEM;
	numa_bitmask_setbit(ctx_params.bmp, 0);
	numa_bitmask_setbit(ctx_params.bmp, 1);
	ctx_params.cap = &cap;
	ctx_params.ctx_set_num = ctx_nums;

	ret = wd_cipher_init2_((char *)cfg->alg_name, cfg->sched_policy,
			       cfg->task_type, &ctx_params);
	if (ret) {
		fprintf(stderr, "FAIL: init2 ret=%d\n", ret);
		numa_free_nodemask(ctx_params.bmp);
		return ret;
	}

	g_init2.bmp   = ctx_params.bmp;
	g_init2.ready = 1;
	return 0;
}

static void global_uninit(struct bench_config *cfg)
{
	if (cfg->init_type == BENCH_INIT_OLD) {
		if (!g_old.ready) return;
		wd_cipher_uninit();
		for (int i = 0; i < g_old.nctxs; i++) {
			if (g_old.ctx_cfg.ctxs[i].ctx)
				wd_release_ctx(g_old.ctx_cfg.ctxs[i].ctx);
		}
		free(g_old.ctx_cfg.ctxs);
		g_old.ctx_cfg.ctxs = NULL;
		if (g_old.sched) {
			wd_sched_rr_release(g_old.sched);
			g_old.sched = NULL;
		}
		g_old.ready = 0;
		return;
	}

	if (!g_init2.ready) return;
	wd_cipher_uninit2();
	if (g_init2.bmp) {
		numa_free_nodemask(g_init2.bmp);
		g_init2.bmp = NULL;
	}
	g_init2.ready = 0;
}

/* ── per-session init/cleanup (no global init/uninit) ─────────────── */
static int session_init(struct bench_config *cfg, struct async_ctx *ctx)
{
	struct wd_cipher_sess_setup setup = {0};
	struct sched_params   sp          = {0};

	memset(ctx, 0, sizeof(*ctx));

	ctx->src = mmap(NULL, cfg->pkt_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ctx->dst = mmap(NULL, cfg->pkt_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ctx->src == MAP_FAILED || ctx->dst == MAP_FAILED)
		return -ENOMEM;

	memset(ctx->src, 0xCC, cfg->pkt_size);
	memset(ctx->dst, 0, cfg->pkt_size);
	memset(ctx->key, 0xAA, KEY_SIZE);
	memset(ctx->iv,  0xBB, IV_SIZE);

	if (cfg->init_type == BENCH_INIT_OLD) {
		/* old init discovers aes-ecb from hisi_sec2; match the session */
		setup.alg    = WD_CIPHER_AES;
		setup.mode   = WD_CIPHER_ECB;
	} else {
		setup.alg    = WD_CIPHER_SM4;
		setup.mode   = WD_CIPHER_CTR;
	}
	setup.mm_type = UADK_MEM_AUTO;
	sp.numa_id   = 0;
	sp.type      = 0;
	sp.mode      = 0;
	setup.sched_param = &sp;

	ctx->h_sess = wd_cipher_alloc_sess(&setup);
	if (!ctx->h_sess) { fprintf(stderr, "FAIL: alloc_sess\n"); goto err_mmap; }

	if (wd_cipher_set_key(ctx->h_sess, ctx->key, KEY_SIZE)) {
		fprintf(stderr, "FAIL: set_key\n"); goto err_sess;
	}

	ctx->req.src           = ctx->src;
	ctx->req.dst           = ctx->dst;
	ctx->req.iv            = ctx->iv;
	ctx->req.iv_bytes      = IV_SIZE;
	ctx->req.in_bytes      = cfg->pkt_size;
	ctx->req.out_bytes     = cfg->pkt_size;
	ctx->req.out_buf_bytes = cfg->pkt_size;
	ctx->req.op_type       = WD_CIPHER_ENCRYPTION;
	ctx->req.data_fmt      = 0;
	ctx->req.state         = 0;

	if (cfg->mode == BENCH_ASYNC) {
		ctx->req.cb       = async_cb;
		ctx->req.cb_param = ctx;
	}

	return 0;

err_sess:
	wd_cipher_free_sess(ctx->h_sess);
err_mmap:
	munmap(ctx->src, cfg->pkt_size);
	munmap(ctx->dst, cfg->pkt_size);
	return -1;
}

static void session_cleanup(struct async_ctx *ctx, struct bench_config *cfg)
{
	if (ctx->h_sess)
		wd_cipher_free_sess(ctx->h_sess);
	munmap(ctx->src, cfg->pkt_size);
	munmap(ctx->dst, cfg->pkt_size);
}

/* ── benchmark loops ──────────────────────────────────────────────── */

static void async_run_bench(struct bench_config *cfg, struct async_ctx *ctx,
			    struct thread_stats *st)
{
	struct timeval start, end;
	__u32 _c;
	int try_cnt, ret;
	__u64 total_sent = 0;

	ctx->running   = 1;
	ctx->completed = 0;
	ctx->st        = st;

	gettimeofday(&start, NULL);

	while (ctx->running) {
		try_cnt = 0;
		ctx->send_times[total_sent & (LAT_RING_SIZE - 1)] = now_us();
		ret = wd_do_cipher_async(ctx->h_sess, &ctx->req);
		if (ret >= 0) {
			total_sent++;

			if ((total_sent & 63) == 0)
				do { wd_cipher_poll(32, &_c); } while(0);

			if ((total_sent & 1023) == 0) {
				gettimeofday(&end, NULL);
				if (end.tv_sec - start.tv_sec >= cfg->duration_secs)
					ctx->running = 0;
			}
			continue;
		}
		do { wd_cipher_poll(16, &_c); } while(0);
		if (++try_cnt > 1000) { usleep(10); try_cnt = 0; }
		gettimeofday(&end, NULL);
		if (end.tv_sec - start.tv_sec >= cfg->duration_secs)
			ctx->running = 0;
	}

	gettimeofday(&end, NULL);

	/* drain */
	{
		int drain_timeout = 5000;
		while (__sync_fetch_and_add(&ctx->completed, 0) < total_sent
		       && drain_timeout-- > 0) {
			usleep(1000);
			do { wd_cipher_poll(64, &_c); } while(0);
		}
	}

	st->elapsed_sec       = elapsed_sec_since(&start);
	st->total_sent        = total_sent;
	st->total_completed   = __sync_fetch_and_add(&ctx->completed, 0);
}

static void sync_run_bench(struct bench_config *cfg, struct async_ctx *ctx,
			   struct thread_stats *st)
{
	struct timeval start, end;
	__u64 total_sent = 0;
	int ret;

	gettimeofday(&start, NULL);

	while (1) {
		__u64 t1 = now_us();

		ret = wd_do_cipher_sync(ctx->h_sess, &ctx->req);
		if (ret < 0) {
			usleep(10);
			gettimeofday(&end, NULL);
			if (end.tv_sec - start.tv_sec >= cfg->duration_secs)
				break;
			continue;
		}

		__u64 t2 = now_us();
		total_sent++;

		if (st->lat_count < LAT_SAMPLE_MAX && (total_sent & 127) == 0)
			st->lat_samples[st->lat_count++] = t2 - t1;

		if ((total_sent & 1023) == 0) {
			gettimeofday(&end, NULL);
			if (end.tv_sec - start.tv_sec >= cfg->duration_secs)
				break;
		}
	}

	gettimeofday(&end, NULL);

	st->elapsed_sec       = elapsed_sec_since(&start);
	st->total_sent        = total_sent;
	st->total_completed   = total_sent;
}

/* ── single benchmark run (session init → bench → session cleanup) ── */
static int run_benchmark(struct bench_config *cfg, struct thread_stats *st)
{
	struct async_ctx ctx;
	int ret;

	memset(st, 0, sizeof(*st));

	ret = session_init(cfg, &ctx);
	if (ret) return ret;

	if (cfg->mode == BENCH_SYNC)
		sync_run_bench(cfg, &ctx, st);
	else
		async_run_bench(cfg, &ctx, st);

	session_cleanup(&ctx, cfg);
	return 0;
}

/* ── multi-threaded dispatch ───────────────────────────────────────── */
static void *thread_worker(void *arg)
{
	struct bench_config *cfg = (struct bench_config *)arg;
	struct thread_stats  *st  = calloc(1, sizeof(*st));

	if (!st)
		return NULL;

	run_benchmark(cfg, st);
	return st;
}

static int run_multi_threaded(struct bench_config *cfg)
{
	pthread_t  tids[MAX_THREADS];
	struct thread_stats *stats[MAX_THREADS];
	struct thread_stats  sum;
	double     kops;
	int        i, ret;

	memset(stats, 0, sizeof(stats));
	memset(&sum, 0, sizeof(sum));

	for (i = 0; i < cfg->nthreads; i++) {
		ret = pthread_create(&tids[i], NULL, thread_worker, cfg);
		if (ret) {
			fprintf(stderr, "FAIL: pthread_create thread=%d err=%d\n", i, ret);
			return -1;
		}
	}

	for (i = 0; i < cfg->nthreads; i++) {
		pthread_join(tids[i], (void **)&stats[i]);
		if (!stats[i])
			fprintf(stderr, "WARN: thread %d returned NULL\n", i);
	}

	for (i = 0; i < cfg->nthreads; i++) {
		if (!stats[i]) continue;
		sum.total_sent      += stats[i]->total_sent;
		sum.total_completed += stats[i]->total_completed;
		if (stats[i]->elapsed_sec > sum.elapsed_sec)
			sum.elapsed_sec = stats[i]->elapsed_sec;
		if (i == 0 && stats[i]->lat_count > 0) {
			sum.lat_count = stats[i]->lat_count;
			memcpy(sum.lat_samples, stats[i]->lat_samples,
			       sum.lat_count * sizeof(__u64));
		}
		free(stats[i]);
	}

	kops = sum.total_sent / 1000.0 / sum.elapsed_sec;

	fprintf(stderr, "  Threads: %d  Sent: %llu  Completed: %llu  Time: %.2fs  Kops: %.1f\n",
		cfg->nthreads,
		(unsigned long long)sum.total_sent,
		(unsigned long long)sum.total_completed,
		sum.elapsed_sec, kops);

	if (sum.lat_count > 0)
		print_latency_stats(&sum);

	return 0;
}

/* ── CLI ───────────────────────────────────────────────────────────── */
static const char *policy_name(int p)
{
	switch (p) {
	case SCHED_POLICY_RR:     return "RR";
	case SCHED_POLICY_LOOP:   return "LOOP";
	case SCHED_POLICY_HUNGRY: return "HUNGRY";
	case SCHED_POLICY_INSTR:  return "INSTR";
	default:                 return "?";
	}
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -m, --mode      sync|async      (default: async)\n"
		"  -i, --init      old|init2       (default: init2)\n"
		"  -s, --sched     rr|loop|hungry|instr (default: rr)\n"
		"  -t, --task      hw|mix          (default: hw)\n"
		"  -j, --threads   N               (default: 1)\n"
		"  -d, --duration  SECS            (default: 3)\n"
		"  -p, --pkt-size  BYTES           (default: 4096)\n"
		"  --ctx-nums      N               (default: 16)\n"
		"  --msg-num       N               (default: 2048)\n"
		"  --show-prop-stats                Show per-prop distribution stats\n"
		"  -h, --help\n",
		prog);
}

int main(int argc, char **argv)
{
	struct bench_config cfg = {
		.mode            = BENCH_ASYNC,
		.init_type       = BENCH_INIT2,
		.sched_policy    = SCHED_POLICY_RR,
		.task_type       = TASK_HW,
		.nthreads        = 1,
		.duration_secs   = BENCH_SECS,
		.pkt_size        = PKT_LEN,
		.ctx_nums        = CTX_NUMS,
		.alg_name        = "ctr(sm4)",
		.ctx_msg_num     = 2048,
		.show_prop_stats = false,
	};
	const char *mode_str = "async";
	const char *init_str = "init2";
	const char *task_str = "HW";

	static struct option long_opts[] = {
		{"mode",     required_argument, 0, 'm'},
		{"init",     required_argument, 0, 'i'},
		{"sched",    required_argument, 0, 's'},
		{"task",     required_argument, 0, 't'},
		{"threads",  required_argument, 0, 'j'},
		{"duration", required_argument, 0, 'd'},
		{"pkt-size", required_argument, 0, 'p'},
		{"ctx-nums", required_argument, 0, 256},
		{"msg-num",         required_argument, 0, 257},
		{"show-prop-stats", no_argument,       0, 258},
		{"help",            no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};
	int opt;

	while ((opt = getopt_long(argc, argv, "m:i:s:t:j:d:p:h",
				  long_opts, NULL)) != -1) {
		switch (opt) {
		case 'm':
			if (!strcmp(optarg, "sync")) { cfg.mode = BENCH_SYNC;  mode_str = "sync"; }
			else                          { cfg.mode = BENCH_ASYNC; mode_str = "async"; }
			break;
		case 'i':
			if (!strcmp(optarg, "old"))  { cfg.init_type = BENCH_INIT_OLD; init_str = "old"; }
			else                          { cfg.init_type = BENCH_INIT2;    init_str = "init2"; }
			break;
		case 's':
			if (!strcmp(optarg, "loop"))      { cfg.sched_policy = SCHED_POLICY_LOOP; }
			else if (!strcmp(optarg, "hungry")){ cfg.sched_policy = SCHED_POLICY_HUNGRY; }
			else if (!strcmp(optarg, "instr")) { cfg.sched_policy = SCHED_POLICY_INSTR; }
			else                               { cfg.sched_policy = SCHED_POLICY_RR; }
			break;
		case 't':
			if (!strcmp(optarg, "mix")) { cfg.task_type = TASK_MIX; task_str = "MIX"; }
			else                        { cfg.task_type = TASK_HW;  task_str = "HW"; }
			break;
		case 'j':
			cfg.nthreads = atoi(optarg);
			if (cfg.nthreads < 1) cfg.nthreads = 1;
			if (cfg.nthreads > MAX_THREADS) cfg.nthreads = MAX_THREADS;
			break;
		case 'd':
			cfg.duration_secs = atoi(optarg);
			if (cfg.duration_secs < 1) cfg.duration_secs = 1;
			break;
		case 'p':
			cfg.pkt_size = atoi(optarg);
			if (cfg.pkt_size < 16) cfg.pkt_size = 16;
			break;
		case 256:
			cfg.ctx_nums = atoi(optarg);
			if (cfg.ctx_nums < 1) cfg.ctx_nums = 1;
			break;
		case 257:
			cfg.ctx_msg_num = atoi(optarg);
			break;
		case 258:
			cfg.show_prop_stats = true;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return 1;
		}
	}

	setbuf(stderr, NULL);

	fprintf(stderr, "\n=== %-8s  %-5s  %s  %s  j=%d  (CTX=%d) ===\n",
		policy_name(cfg.sched_policy), task_str, mode_str, init_str,
		cfg.nthreads, cfg.ctx_nums);

	if (cfg.init_type == BENCH_INIT_OLD && cfg.task_type == TASK_MIX) {
		fprintf(stderr, "WARN: old init does not support TASK_MIX, falling back to HW\n");
		cfg.task_type = TASK_HW;
	}

	/* global init once */
	if (global_init(&cfg) != 0)
		return 1;

	if (cfg.nthreads == 1) {
		struct thread_stats st;
		int ret = run_benchmark(&cfg, &st);
		if (ret) { global_uninit(&cfg); return ret; }
		double kops = st.total_sent / 1000.0 / st.elapsed_sec;
		fprintf(stderr, "  Sent: %llu  Completed: %llu  Time: %.2fs  Kops: %.1f\n",
			(unsigned long long)st.total_sent,
			(unsigned long long)st.total_completed,
			st.elapsed_sec, kops);
		print_latency_stats(&st);
	} else {
		run_multi_threaded(&cfg);
	}

	if (cfg.show_prop_stats)
		fprintf(stderr, "  --- Prop distribution (from SCHED_UNINIT_STATS below) ---\n");
	global_uninit(&cfg);
	return 0;
}
