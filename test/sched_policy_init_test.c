/*
 * sched_policy_init_test.c — Full cipher verification via OLD wd_cipher_init() API
 *
 * Verifies the original init path (wd_sched_rr_alloc + wd_cipher_init) is not
 * broken by the new scheduling policy changes. Tests RR/DEV/LOOP/HUNGRY policies
 * with all AES cipher modes via HW accelerator.
 *
 * Compile:
 *   gcc -std=gnu11 -Wall -O2 -I/usr/local/include/uadk -pthread \
 *       -o sched_policy_init_test sched_policy_init_test.c \
 *       -L/usr/local/lib -lwd -lwd_crypto -lnuma -lpthread -ldl
 * Run:
 *   LD_LIBRARY_PATH=/usr/local/lib ./sched_policy_init_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <numa.h>
#include "wd.h"
#include "wd_alg.h"
#include "wd_alg_common.h"
#include "wd_sched.h"
#include "wd_cipher.h"

#define N_CTX 4  /* sync=2 async=2 per op_type */

static struct wd_ctx_config g_ctx_cfg;
static struct wd_sched *g_sched;

static int parse_mode_enum(const char *m, enum wd_cipher_mode *out)
{
	if (!strcmp(m, "ecb")) { *out = WD_CIPHER_ECB; return 0; }
	if (!strcmp(m, "cbc")) { *out = WD_CIPHER_CBC; return 0; }
	if (!strcmp(m, "ctr")) { *out = WD_CIPHER_CTR; return 0; }
	if (!strcmp(m, "xts")) { *out = WD_CIPHER_XTS; return 0; }
	if (!strcmp(m, "ofb")) { *out = WD_CIPHER_OFB; return 0; }
	if (!strcmp(m, "cfb")) { *out = WD_CIPHER_CFB; return 0; }
	return -1;
}

/* Old init: allocate ctx_config + sched, call wd_cipher_init */
static int init_old_api(__u32 sched_policy, const char *alg_class)
{
	int max_node = numa_max_node() + 1;
	struct sched_params param = {0};
	int ret;

	memset(&g_ctx_cfg, 0, sizeof(g_ctx_cfg));
	g_ctx_cfg.ctx_num = N_CTX;
	g_ctx_cfg.ctxs = calloc(N_CTX, sizeof(struct wd_ctx));
	if (!g_ctx_cfg.ctxs) return -1;

	for (int i = 0; i < N_CTX; i++) {
		struct uacce_dev *dev = wd_get_accel_dev(alg_class);
		if (!dev) {
			fprintf(stderr, "  init_old: failed to get %s device\n", alg_class);
			ret = -1; goto free_ctxs;
		}
		if (dev->numa_id < 0) dev->numa_id = 0;
		g_ctx_cfg.ctxs[i].ctx = wd_request_ctx(dev);
		if (!g_ctx_cfg.ctxs[i].ctx) {
			fprintf(stderr, "  init_old: failed to request ctx %d\n", i);
			ret = -1; goto free_ctx;
		}
		g_ctx_cfg.ctxs[i].op_type = (i < N_CTX/2) ? 0 : 1;
	}

	g_sched = wd_sched_rr_alloc(sched_policy, 2, max_node, wd_cipher_poll_ctx);
	if (!g_sched) {
		fprintf(stderr, "  init_old: wd_sched_rr_alloc failed\n");
		ret = -1; goto free_ctx;
	}

	param.numa_id = 0;
	param.type = 0;
	param.mode = 0;
	param.begin = 0;
	param.end = N_CTX - 1;
	param.dev_id = 0;
	ret = wd_sched_rr_instance(g_sched, &param);
	if (ret) {
		fprintf(stderr, "  init_old: wd_sched_rr_instance failed (%d)\n", ret);
		goto release_sched;
	}

	ret = wd_cipher_init(&g_ctx_cfg, g_sched);
	if (ret) {
		fprintf(stderr, "  init_old: wd_cipher_init failed (%d)\n", ret);
		goto release_sched;
	}

	return 0;

release_sched:
	wd_sched_rr_release(g_sched);
	g_sched = NULL;
free_ctx:
	for (int i = 0; i < N_CTX; i++) {
		if (g_ctx_cfg.ctxs[i].ctx) wd_release_ctx(g_ctx_cfg.ctxs[i].ctx);
	}
free_ctxs:
	free(g_ctx_cfg.ctxs);
	g_ctx_cfg.ctxs = NULL;
	return ret;
}

static void uninit_old_api(void)
{
	wd_cipher_uninit();
	for (int i = 0; i < N_CTX; i++)
		wd_release_ctx(g_ctx_cfg.ctxs[i].ctx);
	free(g_ctx_cfg.ctxs);
	g_ctx_cfg.ctxs = NULL;
	if (g_sched) {
		wd_sched_rr_release(g_sched);
		g_sched = NULL;
	}
}

static int test_cipher(const char *mode_str, enum wd_cipher_alg alg_enum,
                       enum wd_cipher_mode mode_enum)
{
	struct sched_params sp = {.numa_id = 0, .type = 0, .mode = 0};
	struct wd_cipher_sess_setup setup = {
		.alg = alg_enum, .mode = mode_enum, .sched_param = &sp,
	};
	handle_t h = wd_cipher_alloc_sess(&setup);
	if (!h) return -1;

	int is_xts = !strcmp(mode_str, "xts");
	int key_len = is_xts ? 32 : 16;
	__u8 key[32]; memset(key, 0xAA, key_len);
	if (wd_cipher_set_key(h, key, key_len) != 0) {
		wd_cipher_free_sess(h); return -1;
	}

	__u8 iv[16] = {0}, in[1024], out[1024];
	memset(in, 0xBB, sizeof(in)); memset(out, 0, sizeof(out));

	int ok = 0;
	for (int i = 0; i < 64; i++) {
		struct wd_cipher_req req = {
			.src = in, .dst = out,
			.in_bytes = sizeof(in), .out_buf_bytes = sizeof(out),
			.out_bytes = sizeof(out),
			.iv = iv, .iv_bytes = 16,
			.op_type = WD_CIPHER_ENCRYPTION, .data_fmt = WD_FLAT_BUF,
		};
		if (wd_do_cipher_sync(h, &req) == 0) ok++;
	}

	wd_cipher_free_sess(h);
	return (ok == 64) ? 0 : -1;
}

static int test_policy(__u32 sched_policy, const char *pol_name)
{
	const char *modes[] = {"ecb","cbc","ctr","xts","ofb","cfb",NULL};
	int pass = 0, fail = 0;

	printf("  --- %s policy (old init) ---\n", pol_name);
	fflush(stdout);

	if (init_old_api(sched_policy, "cipher") != 0) {
		printf("  [%s] SKIP (init failed)\n", pol_name);
		return 1;
	}

	for (int i = 0; modes[i]; i++) {
		enum wd_cipher_mode mode_enum = WD_CIPHER_ECB;
		parse_mode_enum(modes[i], &mode_enum);
		int ret = test_cipher(modes[i], WD_CIPHER_AES, mode_enum);
		if (ret == 0) {
			printf("  [%s | %-5s | AES-HW] OK (64/64)\n", pol_name, modes[i]);
			pass++;
		} else {
			printf("  [%s | %-5s | AES-HW] FAIL\n", pol_name, modes[i]);
			fail++;
		}
		fflush(stdout);
	}

	uninit_old_api();
	usleep(100000);

	printf("  => %s: %d pass, %d fail\n\n", pol_name, pass, fail);
	return fail;
}

int main(void)
{
	int total_fail = 0;

	printf("=== Old wd_cipher_init() API Verification ===\n");
	printf("Verifying 6 AES modes x RR/DEV/LOOP/HUNGRY via old init path\n\n");

	usleep(200000);

	total_fail += test_policy(SCHED_POLICY_RR,  "RR");
	total_fail += test_policy(SCHED_POLICY_DEV, "DEV");
	total_fail += test_policy(SCHED_POLICY_LOOP, "LOOP");
	total_fail += test_policy(SCHED_POLICY_HUNGRY, "HUNGRY");

	printf("=== Summary ===\n");
	printf("Total failures: %d\n", total_fail);
	if (total_fail == 0)
		printf("Old init API works correctly with all policies!\n");
	else
		printf("Some tests failed — old init API may be broken.\n");

	return total_fail > 0 ? 1 : 0;
}
