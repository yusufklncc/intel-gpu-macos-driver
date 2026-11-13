// SPDX-License-Identifier: MIT
// tlbinring_test: offline check of the in-ring TLB wait's pure parts (xe_tlbinring.h), no GPU needed.
//   1. xeTlbInringPlan: which pending in-fences an EXEC may wait for in the ring (TLB invalidations only), the target
//      (the newest, across the seqno wrap), and the targets an unsigned compare cannot take (0, the guard below the wrap,
//      a word on the other side of the wrap)
//   2. xeTlbInringPath: when the job goes to the armed slot (the queue stays in direct submission), to the normal ring
//      (knob 2) or the old way (FIFO, leaving direct submission), against a brute-force statement of the rules
//   3. a model of the GPU's MI_SEMAPHORE_WAIT (unsigned word >= target) and the TLB word (xeTlbWordAdvances from racing,
//      stale publishers), around the 2^32 wrap: for every accepted job and every word value the GPU can read while
//      the job is pending, the semaphore passes exactly when its invalidations are done in seqno order (never early,
//      never stranded); the word never moves back and is never ahead of the done seqno; with a jump past the documented
//      bound (2^20 unanswered invalidations) the wait can strand a job but still never releases it early
//   4. xeTlbDoneBoth (the word's value) is done for seqno s exactly when tlbSeqDone is (primary and, while it is up,
//      the media GuC)
//   5. what the free/reuse paths still wait for: an unbind's deferred free (xeDeferReady) stays held until the CPU-side
//      done seqno passes its invalidation and the VM's jobs of that moment complete, whatever the EXECs that wait for
//      that invalidation in the ring do; and those EXECs' batches never start before the same invalidation is done
// c++ -std=c++17 -Wall -Wextra -I.. -o tlbinring_test tlbinring_test.cpp && ./tlbinring_test
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "xe_tlbinring.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static XeFence tlb(uint32_t s) { return xeTlbFence(s); }
static XeFence job(uint32_t slot, uint32_t s) { return XeFence{0, slot, 1, s}; }
static bool semPass(uint32_t word, uint32_t target) { return word >= target; }   // COMPARE_OP_SAD_GTE_SDD, unsigned
static int gAccepted, gRefused;

int main() {
	srandom(12);
	// 1. the plan
	{
		uint32_t t = 7;
		CHECK(xeTlbInringPlan(nullptr, 0, 0, &t) == kXeTlbInringNone, "no in-fences");
		XeFence j1[] = {job(3, 10), XeFence{0, kXeBindSlot, 2, 4}};
		CHECK(xeTlbInringPlan(j1, 2, 0, &t) == kXeTlbInringNone, "jobs and a queued bind only: nothing for this path");
		XeFence m[] = {tlb(100), job(3, 10)};
		CHECK(xeTlbInringPlan(m, 2, 90, &t) == kXeTlbInringOther && t == 100, "a TLB fence and a job: the FIFO");
		XeFence m2[] = {job(3, 10), tlb(100)};
		CHECK(xeTlbInringPlan(m2, 2, 90, &t) == kXeTlbInringOther, "order does not matter");
		XeFence a[] = {tlb(100), tlb(105), tlb(103)};
		CHECK(xeTlbInringPlan(a, 3, 99, &t) == kXeTlbInringWait && t == 105, "newest of several: %u", t);
		CHECK(xeTlbInringPlan(a, 3, 105, &t) == kXeTlbInringWait, "word at the target: passes at once");
		CHECK(xeTlbInringPlan(a, 3, 200, &t) == kXeTlbInringWait, "word past the target, no wrap between");
		XeFence g[] = {tlb(kXeTlbWordGuard - 1)};
		CHECK(xeTlbInringPlan(g, 1, kXeTlbWordGuard - 10, &t) == kXeTlbInringWait, "just below the guard");
		XeFence g2[] = {tlb(kXeTlbWordGuard)};
		CHECK(xeTlbInringPlan(g2, 1, kXeTlbWordGuard - 10, &t) == kXeTlbInringWrap, "at the guard");
		XeFence g3[] = {tlb(0xfffffffeu)};
		CHECK(xeTlbInringPlan(g3, 1, 0xfffffff0u, &t) == kXeTlbInringWrap, "just before the wrap");
		XeFence z[] = {tlb(0)};
		CHECK(xeTlbInringPlan(z, 1, 0xfffffff0u, &t) == kXeTlbInringWrap, "target 0 (no wait encoded as 0)");
		XeFence w[] = {tlb(5)};
		CHECK(xeTlbInringPlan(w, 1, 0xfffffff0u, &t) == kXeTlbInringWrap, "word before the wrap, target after it");
		CHECK(xeTlbInringPlan(w, 1, 2, &t) == kXeTlbInringWait && t == 5, "both after the wrap");
		XeFence wr[] = {tlb(0xfffffff8u), tlb(3)};   // 3 is newer (wrap-aware)
		CHECK(xeTlbInringPlan(wr, 2, 1, &t) == kXeTlbInringWait && t == 3, "newest across the wrap: %u", t);
		XeFence far[] = {tlb(0x10)};   // word 0x80000020: seqno order says the word is behind, unsigned says ahead
		CHECK(xeTlbInringPlan(far, 1, 0x80000020u, &t) == kXeTlbInringWrap, "orders disagree");
	}
	// 2. the path, against the rules written out
	for (int knob = -1; knob <= 3; knob++)
		for (uint32_t plan = 0; plan < 4; plan++)
			for (int bits = 0; bits < 8; bits++) {
				const bool word = bits & 1, armed = bits & 2, free = bits & 4;
				const XeTlbPath p = xeTlbInringPath(knob, (XeTlbInring)plan, word, armed, free);
				XeTlbPath want = kXeTlbPathOld;
				if (knob >= 1 && plan == kXeTlbInringWait && word && free) {
					if (armed) want = kXeTlbPathDirect;            // knob 1 and 2: the armed queue stays armed
					else if (knob >= 2) want = kXeTlbPathRing;     // knob 2: the normal ring path too
				}
				CHECK(p == want, "knob %d plan %u word %d armed %d free %d: %u, want %u", knob, plan, word, armed, free, p, want);
			}
	// 3. the GPU model around the wrap
	auto model = [](uint32_t base, uint32_t maxJump, int steps, bool expectExact) {
		uint32_t issued = base, done = base, word = base;   // seqnos: issued, CPU-side done, the TLB word
		struct Job { uint32_t target; bool passed; };
		std::vector<Job> jobs;
		std::vector<uint32_t> stale;   // values publishers computed earlier and may still store (racing)
		int strandedSeen = 0, early = 0, accepted = 0, refused = 0;
		for (int i = 0; i < steps; i++) {
			const int op = (int)(random() % 4);
			if (op == 0) {   // VM_BINDs issue invalidations
				issued += 1 + (uint32_t)(random() % 8);
			} else if (op == 1 && done != issued) {   // TLB_INVALIDATION_DONE (in order, may cover several)
				uint32_t step = 1 + (uint32_t)(random() % maxJump);
				if (step > issued - done) step = issued - done;
				done += step;
				stale.push_back(done);
			} else if (op == 2) {   // a publisher stores what it computed (maybe long ago): only forward
				if (!stale.empty()) {
					const size_t k = (size_t)random() % stale.size();
					const uint32_t v = stale[k];
					if (xeTlbWordAdvances(word, v)) word = v;
					stale.erase(stale.begin() + (long)k);
				}
			} else if (done != issued) {   // an EXEC on the newest pending invalidation (or an older one)
				const uint32_t s = issued - (uint32_t)(random() % (issued - done));
				XeFence f[] = {tlb(s)};
				uint32_t t;
				if (xeTlbInringPlan(f, 1, word, &t) == kXeTlbInringWait) {
					jobs.push_back({t, false});
					accepted++;
				} else {
					refused++;
				}
			}
			CHECK(xeSeqDone(done, word), "the word %u is ahead of done %u", word, done);
			for (Job &j : jobs) {   // the GPU polls: every value the word takes is read
				const bool pass = semPass(word, j.target), isDone = xeSeqDone(word, j.target);
				if (pass && !xeSeqDone(done, j.target)) early++;
				if (isDone && !pass) strandedSeen++;
				if (pass) j.passed = true;
			}
			std::vector<Job> keep;
			for (const Job &j : jobs) if (!j.passed) keep.push_back(j);
			jobs.swap(keep);
		}
		// drain: everything answered and published
		done = issued;
		if (xeTlbWordAdvances(word, done)) word = done;
		for (const Job &j : jobs) if (!semPass(word, j.target)) strandedSeen++;
		CHECK(early == 0, "base 0x%x: %d waits passed before their invalidation was done", base, early);
		if (expectExact) CHECK(strandedSeen == 0, "base 0x%x: %d waits stranded", base, strandedSeen);
		gAccepted += accepted;
		gRefused += refused;
		return strandedSeen;
	};
	int stranded = 0;
	for (int r = 0; r < 60; r++) {   // across the wrap, across the guard, anywhere
		const uint32_t base = r < 20 ? 0xffffffffu - (uint32_t)(random() % 200000) :
			r < 40 ? kXeTlbWordGuard - (uint32_t)(random() % 30000) : (uint32_t)random();
		model(base, 1000, 60000, true);   // completions of up to 1000 at once: far below the bound
	}
	printf("model: %d EXECs waited in the ring, %d refused (wrap guard)\n", gAccepted, gRefused);
	CHECK(gAccepted > 100000 && gRefused > 0, "the model exercised both");
	// past the bound: one completion of more than 2^20 can strand a waiter whose target was just below the guard;
	// never an early release
	{
		uint32_t word = kXeTlbWordGuard - 0x100, t;
		XeFence f[] = {tlb(kXeTlbWordGuard - 1)};
		CHECK(xeTlbInringPlan(f, 1, word, &t) == kXeTlbInringWait, "accepted below the guard");
		const uint32_t v = t + (1u << 20) + 5;   // wraps past 0 in one step
		CHECK(xeTlbWordAdvances(word, v), "forward in seqno order");
		word = v;
		stranded = !semPass(word, t);
		CHECK(stranded, "documented limit: a jump of 2^20 strands it (the watchdog's GT reset, never an early start)");
		CHECK(!semPass(kXeTlbWordGuard - 2, t), "never early");
	}
	// 4. xeTlbDoneBoth against tlbSeqDone's rule
	for (int i = 0; i < 200000; i++) {
		const uint32_t p = (uint32_t)random() ^ (uint32_t)random() << 16, m = p + (uint32_t)(random() % 2001) - 1000;
		const uint32_t s = p + (uint32_t)(random() % 4001) - 2000;
		const bool media = random() & 1;
		const bool want = xeSeqDone(p, s) && (!media || xeSeqDone(m, s));
		CHECK(xeSeqDone(xeTlbDoneBoth(p, m, media), s) == want, "p %u m %u media %d s %u", p, m, media, s);
	}
	// 5. the free/reuse paths: an unbind seals its free with invalidation S and the VM's jobs of that moment (q0 up to
	// seqno 50); two EXECs after it wait for S in the ring; nothing of what the EXECs do changes when the free may go
	{
		const uint32_t S = 1000;
		uint32_t tlbDone = 998, q0Done = 40, word = 998;
		const uint32_t snapQ0 = 50;
		std::vector<uint32_t> execTargets;
		for (int e = 0; e < 2; e++) {
			XeFence f[] = {tlb(S)};
			uint32_t t;
			CHECK(xeTlbInringPlan(f, 1, word, &t) == kXeTlbInringWait && t == S, "exec %d waits in the ring", e);
			execTargets.push_back(t);
		}
		auto ready = [&] { return xeDeferReady(true, xeSeqDone(tlbDone, S), xeSeqDone(q0Done, snapQ0)); };
		auto batchMayRun = [&](uint32_t t) { return semPass(word, t); };
		CHECK(!ready(), "free held: invalidation and jobs pending");
		for (uint32_t t : execTargets) CHECK(!batchMayRun(t), "EXEC batch held by the semaphore");
		q0Done = 50;   // the VM's jobs of the unbind's moment complete
		CHECK(!ready(), "free still held: the invalidation is pending (the EXECs being in the ring changes nothing)");
		tlbDone = 1000;   // TLB_INVALIDATION_DONE; the publish has not happened yet
		CHECK(ready(), "free goes once the CPU-side done passes S and the snapshot's jobs are done");
		for (uint32_t t : execTargets) CHECK(!batchMayRun(t), "the word lags: the batch still waits (late, never early)");
		word = tlbDone;   // xeTlbWordPublish
		for (uint32_t t : execTargets) CHECK(batchMayRun(t), "after the publish the batches run");
		q0Done = 40;
		CHECK(!ready(), "and the other way round: invalidation done, jobs pending: held");
	}
	printf("tlbinring_test: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
