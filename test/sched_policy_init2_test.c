/*
 * sched_policy_init2_test.c — Single cipher test using wd_cipher_init2_() API
 *
 * Tests a single algo×policy combination in one process. Designed to be called
 * from sched_policy_test.sh for per-test process isolation (avoids HW resource
 * conflicts when switching scheduler types on the same accelerator).
 *
 * Usage:
 *   ./sched_policy_init2_test <algo_name> <policy> <task_type> <ctx_prop> <policy_label> <driver_label>
 *
 *   algo_name: e.g. "cbc(aes)", "ecb(sm4)", "xts(aes)"
 *   policy:    4=LOOP, 5=HUNGRY, 6=INSTR
 *   task_type: 1=TASK_HW, 2=TASK_INSTR
 *   ctx_prop:  0=UADK_CTX_HW, 1=UADK_CTX_CE_INS
 *
 * Compile:
 *   gcc -std=gnu11 -Wall -O2 -I/usr/local/include/uadk -pthread \
 *       -o sched_policy_init2_test sched_policy_init2_test.c \
 *       -L/usr/local/lib -lwd -lwd_crypto -lnuma -lpthread -ldl
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

/* Parse "mode(algo)" string, e.g. "cbc(aes)" → mode=CBC, alg=AES */
static int parse_algo_mode(const char *name, enum wd_cipher_alg *alg, enum wd_cipher_mode *mode)
{
	char mode_str[16] = {0};
	const char *paren = strchr(name, '(');
	if (!paren || (paren - name) >= (int)sizeof(mode_str)) return -1;
	memcpy(mode_str, name, paren - name);

	if (!strcmp(mode_str, "ecb"))      *mode = WD_CIPHER_ECB;
	else if (!strcmp(mode_str, "cbc")) *mode = WD_CIPHER_CBC;
	else if (!strcmp(mode_str, "ctr")) *mode = WD_CIPHER_CTR;
	else if (!strcmp(mode_str, "xts")) *mode = WD_CIPHER_XTS;
	else if (!strcmp(mode_str, "ofb")) *mode = WD_CIPHER_OFB;
	else if (!strcmp(mode_str, "cfb")) *mode = WD_CIPHER_CFB;
	else return -1;

	char algo_str[16] = {0};
	const char *end = strchr(paren + 1, ')');
	if (!end || (end - paren - 1) >= (int)sizeof(algo_str)) return -1;
	memcpy(algo_str, paren + 1, end - paren - 1);

	if (!strcmp(algo_str, "sm4"))       *alg = WD_CIPHER_SM4;
	else if (!strcmp(algo_str, "aes"))  *alg = WD_CIPHER_AES;
	else if (!strcmp(algo_str, "des"))  *alg = WD_CIPHER_DES;
	else if (!strcmp(algo_str, "3des")) *alg = WD_CIPHER_3DES;
	else return -1;

	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 7) {
		fprintf(stderr, "Usage: %s <algo_name> <policy> <task_type> <ctx_prop> <policy_label> <driver_label>\n", argv[0]);
		fprintf(stderr, "  algo_name: e.g. \"cbc(aes)\", \"ecb(sm4)\"\n");
		fprintf(stderr, "  policy: 4=LOOP, 5=HUNGRY, 6=INSTR\n");
		fprintf(stderr, "  task_type: 1=TASK_HW, 2=TASK_INSTR\n");
		fprintf(stderr, "  ctx_prop: 0=HW, 1=CE_INS\n");
		return 2;
	}

	const char *algo_name    = argv[1];
	int         policy       = atoi(argv[2]);
	int         task_type    = atoi(argv[3]);
	int         ctx_prop     = atoi(argv[4]);
	const char *policy_label = argv[5];
	const char *driver_label = argv[6];

	enum wd_cipher_alg  alg_enum;
	enum wd_cipher_mode mode_enum;
	if (parse_algo_mode(algo_name, &alg_enum, &mode_enum) != 0) {
		fprintf(stderr, "%s: cannot parse algo_name '%s'\n", argv[0], algo_name);
		return 2;
	}

	char mode_str[16] = {0};
	const char *paren = strchr(algo_name, '(');
	if (paren) memcpy(mode_str, algo_name, paren - algo_name);

	usleep(50000);

	struct wd_ctx_params p = {0};
	struct wd_ctx_nums n[2] = {0};
	struct wd_cap_config cap = {.ctx_msg_num = 1024};

	n[0].sync_ctx_num = 1; n[0].async_ctx_num = 1; n[0].ctx_prop = ctx_prop;
	n[1].sync_ctx_num = 1; n[1].async_ctx_num = 1; n[1].ctx_prop = ctx_prop;
	p.op_type_num = 2;
	p.bmp = numa_allocate_nodemask();
	numa_bitmask_setbit(p.bmp, 0);
	p.cap = &cap;
	p.ctx_set_num = &n[0];

	int ret = wd_cipher_init2_(algo_name, policy, task_type, &p);
	if (ret) {
		printf("[%s | %-5s | %s] SKIP (init failed: %d)\n", policy_label, mode_str, driver_label, ret);
		numa_free_nodemask(p.bmp);
		return 1;
	}

	struct sched_params sp = {.numa_id = 0, .type = 0, .ctx_prop = ctx_prop};
	struct wd_cipher_sess_setup setup = {
		.alg = alg_enum, .mode = mode_enum, .sched_param = &sp,
	};
	handle_t h = wd_cipher_alloc_sess(&setup);
	if (!h) {
		printf("[%s | %-5s | %s] FAIL (session alloc)\n", policy_label, mode_str, driver_label);
		wd_cipher_uninit2();
		numa_free_nodemask(p.bmp);
		return 1;
	}

	int is_xts = !strcmp(mode_str, "xts");
	int key_len = is_xts ? 32 : 16;
	__u8 key[32]; memset(key, 0xAA, key_len);
	if (wd_cipher_set_key(h, key, key_len) != 0) {
		printf("[%s | %-5s | %s] FAIL (set_key)\n", policy_label, mode_str, driver_label);
		wd_cipher_free_sess(h);
		wd_cipher_uninit2();
		numa_free_nodemask(p.bmp);
		return 1;
	}

	__u8 iv[16] = {0}, in[1024], out[1024];
	memset(in, 0xBB, sizeof(in)); memset(out, 0, sizeof(out));

	int ok = 0, n_tests = 64;
	for (int i = 0; i < n_tests; i++) {
		struct wd_cipher_req req = {
			.src = in, .dst = out,
			.in_bytes = sizeof(in), .out_buf_bytes = sizeof(out),
			.out_bytes = sizeof(out),
			.iv = iv, .iv_bytes = 16,
			.op_type = WD_CIPHER_ENCRYPTION, .data_fmt = WD_FLAT_BUF,
		};
		if (wd_do_cipher_sync(h, &req) == 0) ok++;
	}

	if (ok == n_tests)
		printf("[%s | %-5s | %s] OK (%d/%d)\n", policy_label, mode_str, driver_label, ok, n_tests);
	else
		printf("[%s | %-5s | %s] FAIL (%d/%d encrypts)\n", policy_label, mode_str, driver_label, ok, n_tests);

	wd_cipher_free_sess(h);
	wd_cipher_uninit2();
	numa_free_nodemask(p.bmp);
	return (ok == n_tests) ? 0 : 1;
}
