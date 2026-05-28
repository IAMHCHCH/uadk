/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <numa.h>
#include <sys/mman.h>

#include "wd_cipher.h"
#include "wd_sched.h"
#include "wd_alg_common.h"

#define CTX_NUMS 16
#define PKT_LEN  4096
#define IV_SIZE  16
#define KEY_SIZE 16

static void *cipher_async_cb(struct wd_cipher_req *req, void *cb_param)
{
	printf("[CB] async callback called\n");
	return NULL;
}

int main(void)
{
	struct wd_ctx_params ctx_params = {0};
	struct wd_ctx_nums ctx_nums[2] = {0};
	struct wd_cap_config cap = {0};
	struct wd_cipher_sess_setup setup = {0};
	struct wd_cipher_req req = {0};
	struct sched_params sp = {0};
	handle_t h_sess;
	__u32 poll_cnt = 0;
	__u8 *src = NULL, *dst = NULL;
	__u8 iv[IV_SIZE] = {0};
	__u8 key[KEY_SIZE] = {0};
	int ret;

	printf("=== UADK Init2 Test ===\n");

	/* Step 1: Init2 */
	cap.ctx_msg_num = 1024;

	ctx_nums[0].sync_ctx_num = CTX_NUMS;
	ctx_nums[0].async_ctx_num = CTX_NUMS;
	ctx_nums[1].sync_ctx_num = CTX_NUMS;
	ctx_nums[1].async_ctx_num = CTX_NUMS;

	ctx_params.op_type_num = 2;
	ctx_params.bmp = numa_allocate_nodemask();
	if (!ctx_params.bmp) {
		printf("FAIL: numa_allocate_nodemask\n");
		return -1;
	}
	numa_bitmask_setbit(ctx_params.bmp, 0);
	numa_bitmask_setbit(ctx_params.bmp, 1);
	ctx_params.cap = &cap;
	ctx_params.ctx_set_num = ctx_nums;

	printf("Calling wd_cipher_init2_(\"ctr(sm4)\", HUNGRY, TASK_HW, ...)\n");
	ret = wd_cipher_init2_("ctr(sm4)", SCHED_POLICY_HUNGRY, TASK_HW, &ctx_params);
	if (ret) {
		printf("FAIL: wd_cipher_init2_ returned %d\n", ret);
		goto out_bmp;
	}
	printf("PASS: wd_cipher_init2_ succeeded\n");

	/* Step 2: Allocate session */
	setup.alg = WD_CIPHER_SM4;
	setup.mode = WD_CIPHER_CTR;
	setup.mm_type = UADK_MEM_AUTO;
	sp.numa_id = 0;
	sp.type = 0;
	sp.mode = 0;
	setup.sched_param = &sp;

	printf("Calling wd_cipher_alloc_sess...\n");
	h_sess = wd_cipher_alloc_sess(&setup);
	if (!h_sess) {
		printf("FAIL: wd_cipher_alloc_sess returned NULL\n");
		goto out_uninit;
	}
	printf("PASS: wd_cipher_alloc_sess succeeded (h_sess=%lx)\n", (unsigned long)h_sess);

	/* Step 3: Set key */
	memset(key, 0xAA, KEY_SIZE);
	memset(iv, 0xBB, IV_SIZE);

	ret = wd_cipher_set_key(h_sess, key, KEY_SIZE);
	if (ret) {
		printf("FAIL: wd_cipher_set_key returned %d\n", ret);
		goto out_free_sess;
	}
	printf("PASS: wd_cipher_set_key succeeded\n");

	/* Step 4: Allocate buffers */
	src = mmap(NULL, PKT_LEN, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	dst = mmap(NULL, PKT_LEN, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (src == MAP_FAILED || dst == MAP_FAILED) {
		printf("FAIL: mmap for buffers\n");
		goto out_free_sess;
	}
	memset(src, 0xCC, PKT_LEN);
	memset(dst, 0, PKT_LEN);

	/* Step 5: Do async cipher */
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
	req.cb = cipher_async_cb;

	printf("Calling wd_do_cipher_async...\n");
	ret = wd_do_cipher_async(h_sess, &req);
	if (ret < 0) {
		printf("FAIL: wd_do_cipher_async returned %d\n", ret);
	} else {
		printf("PASS: wd_do_cipher_async returned %d\n", ret);
	}

	/* Step 6: Poll */
	usleep(100000);  /* 100ms wait */
	printf("Calling wd_cipher_poll...\n");
	ret = wd_cipher_poll(1, &poll_cnt);
	printf("wd_cipher_poll returned %d, count=%u\n", ret, poll_cnt);

out:
	if (src != MAP_FAILED)
		munmap(src, PKT_LEN);
	if (dst != MAP_FAILED)
		munmap(dst, PKT_LEN);

out_free_sess:
	wd_cipher_free_sess(h_sess);
out_uninit:
	wd_cipher_uninit();
out_bmp:
	if (ctx_params.bmp)
		numa_free_nodemask(ctx_params.bmp);
	return ret;
}
