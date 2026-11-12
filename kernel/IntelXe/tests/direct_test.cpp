// SPDX-License-Identifier: MIT
// direct_test: offline check of xe_direct.h (rb8e), no GPU needed. The post-stub direct submission calls between a
// job's cache flush and its seqno store holds what xe_ring_ops.c (v6.18) emits there for a job with a user fence:
//   - render / compute: emit_store_imm_ppgtt_posted (MI_STORE_DATA_IMM | MI_SDI_NUM_QW(1) = 0x10200003, address and
//     value as two dwords each, PPGTT: no MI_SDI_GGTT bit), then MI_BATCH_BUFFER_END
//   - copy / video: emit_flush_dw first (MI_FLUSH_DW | MI_FLUSH_IMM_DW = 0x13000002 and three zero dwords)
//   - no user fence: MI_BATCH_BUFFER_END alone
// and always fits the post-stub's kXeDsPostBytes.
// c++ -std=c++17 -Wall -Wextra -I.. -o direct_test direct_test.cpp && ./direct_test
#include <stdio.h>
#include <stdlib.h>
#include "xe_direct.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

// xe_mi_commands.h (v6.18)
static constexpr uint32_t MI_INSTR(uint32_t op) { return op << 23; }
static constexpr uint32_t MI_BATCH_BUFFER_END = MI_INSTR(0xA), MI_STORE_DATA_IMM = MI_INSTR(0x20), MI_SDI_GGTT = 1u << 22,
	MI_FLUSH_DW = MI_INSTR(0x26), MI_FLUSH_IMM_DW = 4 - 2;
static constexpr uint32_t MI_SDI_NUM_QW(uint32_t x) { return (2 * x + 3 - 2) | 1u << 21; }

int main() {
	uint32_t dw[kXeDsPostBytes / 4 + 8];
	srandom(1);
	for (int r = 0; r < 20000; r++) {
		const bool rc = random() % 2, uf = random() % 4 != 0;
		const uint64_t addr = ((uint64_t)random() << 21 | (uint64_t)random() << 3) & 0xfffffffffff8ull,
			value = (uint64_t)random() << 33 ^ (uint64_t)random();
		for (uint32_t &d : dw) d = 0xdeadbeef;
		const uint32_t n = xeDsPostStub(dw, rc, uf, addr, value);
		CHECK(n * 4 <= kXeDsPostBytes && dw[kXeDsPostBytes / 4] == 0xdeadbeef, "%u dwords: beyond the post-stub", n);
		CHECK(dw[n - 1] == MI_BATCH_BUFFER_END, "round %d: does not end with MI_BATCH_BUFFER_END", r);
		if (!uf) {
			CHECK(n == 1, "no user fence: %u dwords", n);
			continue;
		}
		uint32_t i = 0;
		if (!rc) {
			CHECK(dw[0] == (MI_FLUSH_DW | MI_FLUSH_IMM_DW) && !dw[1] && !dw[2] && !dw[3], "copy/video: emit_flush_dw first");
			i = 4;
		}
		CHECK(dw[i] == (MI_STORE_DATA_IMM | MI_SDI_NUM_QW(1)) && dw[i] == 0x10200003u && !(dw[i] & MI_SDI_GGTT),
			"store header 0x%08x", dw[i]);
		CHECK(dw[i + 1] == (uint32_t)addr && dw[i + 2] == (uint32_t)(addr >> 32), "address");
		CHECK(dw[i + 3] == (uint32_t)value && dw[i + 4] == (uint32_t)(value >> 32), "value");
		CHECK(n == i + 6, "length %u", n);
	}
	printf("direct_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
