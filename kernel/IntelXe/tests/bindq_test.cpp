// SPDX-License-Identifier: MIT
// bindq_test: offline check of rb8f's pure parts (xe_bindq.h), no GPU needed.
//   1. xeBindPath: applied at once only with no in-fence pending and an idle bind queue (xe_pt.c
//      xe_pt_vm_dependencies: no_in_syncs and xe_exec_queue_is_idle, else a job); queued with the switch on, or behind
//      queued calls of the same queue whatever the switch; the old wait without room
//   2. the out-fence records (xeBjAlloc / xeBjPublish / xeBjSignaled): pending until applied and the bind fence the
//      apply produced has signaled; a full ring refuses; a record is reused only once its fence has signaled, and the
//      old fence then still reads signaled; the serial skips 0 across the wrap
//   3. xeBindPick: the oldest call of each bind queue only, in submission order; a ready call behind a blocked one of
//      its queue waits, a ready call of another queue does not
//   4. a model of the worker: random calls on three bind queues, with in-fences on exec-queue seqnos and on earlier
//      queued calls (other queues' and its own), random completions of execs, clears and invalidations: every call is
//      applied once, after its in-fences, each queue's calls in submission order; once a call's out-fence reads
//      signaled so do the earlier calls of its queue (so fenceMerge / surfAdd may keep the later of two fences of one
//      queue), and its own clear and invalidation have completed; everything is applied in the end
//   5. xeUfBusy (xe_vm.c check_ufence): EBUSY only for an op that removes or splits a VMA it meets whose user fence is
//      pending; UNMAP_ALL meets its object's VMAs; PREFETCH never
//   6. xeBindOpSpan / xeBindSpanOverlap: calls of two bind queues are ordered where their ranges overlap
//      (xe_pt_vm_dependencies, the VM's range fences); UNMAP_ALL meets everything, an empty range nothing
//   7. xeUfOrphanAction (xe_sync.c user_fence_worker, mmget_not_zero): written once ready while the process lives,
//      dropped once it is gone, dropped after the bound while pending
// c++ -std=c++17 -Wall -Wextra -I.. -o bindq_test bindq_test.cpp && ./bindq_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "xe_bindq.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

// ─── the fence world of the model ──────────────────────────────────────────────────────────────────────────────────
static XeBjRec gTab[kXeBjMax];
static uint32_t gExecDone[4], gClearDone, gTlbDone;
static const uint32_t kClearSlot = 3;   // the migrate queue: clears, in order
static bool sig(const XeFence &f) {
	if (f.slot == kXeFenceDone) return true;
	if (f.slot == kXeTlbSlot) return xeSeqDone(gTlbDone, f.seqno);
	if (f.slot == kXeBindSlot) return xeBjSignaled(gTab, f, sig);
	return xeSeqDone(gExecDone[f.slot], f.seqno);
}
static const XeFence kDone = {0, kXeFenceDone, 0, 0};

static void testPath() {
	for (int a = 0; a < 2; a++)
		for (int in = 0; in < 2; in++)
			for (uint32_t qp = 0; qp < 2; qp++)
				for (int room = 0; room < 2; room++) {
					const XeBindPathKind k = xeBindPath(a, in, qp, room);
					XeBindPathKind want;
					if (!in && !qp) want = kXeBindNow;
					else if (room && (a || qp)) want = kXeBindQueue;
					else want = kXeBindWait;
					CHECK(k == want, "path async %d in %d queued %u room %d: %d, want %d", a, in, qp, room, k, want);
				}
	CHECK(xeBindPath(true, false, 0, false) == kXeBindNow, "nothing to wait for: applied at once");
	CHECK(xeBindPath(false, false, 3, true) == kXeBindQueue, "behind queued calls with the switch off: queued");
	CHECK(xeBindPath(false, true, 0, true) == kXeBindWait, "switch off, in-fence pending: the old wait");
}

static void testRecords() {
	memset(gTab, 0, sizeof gTab);
	memset(gExecDone, 0, sizeof gExecDone);
	gClearDone = gTlbDone = 0;
	uint32_t next = 1;
	std::vector<uint32_t> s;
	for (uint32_t i = 0; i < kXeBjMax; i++) {
		const uint32_t v = xeBjAlloc(gTab, &next, sig);
		CHECK(v == i + 1, "alloc %u: %u", i, v);
		s.push_back(v);
	}
	CHECK(xeBjAlloc(gTab, &next, sig) == 0, "a full ring refuses");
	const XeFence f0 = xeBindJobFence(7, s[0]);
	CHECK(!sig(f0), "pending before its apply");
	// applied with a bind fence still pending (an invalidation)
	xeBjPublish(gTab, s[0], XeBindFence{kDone, xeTlbFence(5)});
	CHECK(!sig(f0), "applied, invalidation pending: not signaled");
	CHECK(xeBjAlloc(gTab, &next, sig) == 0, "its record is not free while its fence is pending");
	gTlbDone = 5;
	CHECK(sig(f0), "applied, invalidation done: signaled");
	const uint32_t r = xeBjAlloc(gTab, &next, sig);
	CHECK(r == kXeBjMax + 1 && r % kXeBjMax == s[0] % kXeBjMax, "reuse of the first record: %u", r);
	CHECK(sig(f0), "the old fence still reads signaled after its record was reused");
	CHECK(!sig(xeBindJobFence(7, r)), "the new call is pending");
	// wrap: serial 0 is never handed out
	memset(gTab, 0, sizeof gTab);
	next = 0xffffffffu;
	const uint32_t w1 = xeBjAlloc(gTab, &next, sig), w2 = xeBjAlloc(gTab, &next, sig);
	CHECK(w1 == 0xffffffffu && w2 == 1, "across the wrap: %u then %u", w1, w2);
	// a dropped call (published with signaled parts) signals at once
	xeBjPublish(gTab, w1, XeBindFence{kDone, kDone});
	CHECK(sig(xeBindJobFence(1, w1)), "dropped: signaled");
}

struct PNode { PNode *next; int q; bool ready; };
static void testPick() {
	PNode n[5] = {};
	const int qs[5] = {0, 1, 0, 2, 1};
	for (int i = 0; i < 5; i++) {
		n[i].q = qs[i];
		n[i].next = i < 4 ? &n[i + 1] : nullptr;
	}
	auto key = [](PNode *p) { return p->q; };
	auto ready = [](PNode *p) { return p->ready; };
	CHECK(!xeBindPick(&n[0], key, ready), "nothing ready: none");
	n[2].ready = true;   // queue 0's second, behind its blocked first
	CHECK(!xeBindPick(&n[0], key, ready), "a ready call behind a blocked one of its queue waits");
	n[3].ready = true;   // queue 2's first
	CHECK(xeBindPick(&n[0], key, ready) == &n[3], "another queue's ready call goes");
	n[0].ready = true;
	CHECK(xeBindPick(&n[0], key, ready) == &n[0], "submission order among the ready oldest");
	n[4].ready = true;   // queue 1's second: its first (n[1]) is not ready
	n[0].ready = n[3].ready = false;
	CHECK(!xeBindPick(&n[0], key, ready), "queue 1's second waits for its first; queue 0's second for its first");
}

// ─── 4. the worker model ────────────────────────────────────────────────────────────────────────────────────────────
struct Job {
	Job *next;
	int q, idx;
	uint32_t serial;
	std::vector<XeFence> deps;
	bool clear, unbind;
};
static void testModel(unsigned seed) {
	srand(seed);
	memset(gTab, 0, sizeof gTab);
	memset(gExecDone, 0, sizeof gExecDone);
	gClearDone = gTlbDone = 0;
	uint32_t next = 1, execSeq[3] = {0, 0, 0}, clearSeq = 0, tlbSeq = 0;
	XeBindFence last[3] = {{kDone, kDone}, {kDone, kDone}, {kDone, kDone}};
	Job *head = nullptr, **tail = &head;
	std::vector<Job *> all;
	std::vector<std::vector<int>> applied(3);
	std::vector<int> made(3, 0);
	std::vector<XeFence> outs;   // every call's out-fence, in submission order
	std::vector<int> outQ, prevSame;   // the queue of each call, and the call before it on that queue (-1 none)
	int lastIdx[3] = {-1, -1, -1};
	int nApplied = 0, badDeps = 0, badOrder = 0, badMono = 0, badOwn = 0;
	auto step = [&]() {   // random completions
		for (int k = 0; k < 3; k++)
			if (rand() % 3 == 0 && gExecDone[k] != execSeq[k]) gExecDone[k]++;
		if (rand() % 3 == 0 && gClearDone != clearSeq) gClearDone++;
		gExecDone[kClearSlot] = gClearDone;
		if (rand() % 3 == 0 && gTlbDone != tlbSeq) gTlbDone++;
	};
	auto worker = [&]() {
		for (;;) {
			Job *j = xeBindPick(head, [](Job *p) { return p->q; }, [](Job *p) {
				for (const XeFence &d : p->deps)
					if (!sig(d)) return false;
				return true;
			});
			if (!j) return;
			for (const XeFence &d : j->deps) badDeps += !sig(d);
			if (!applied[j->q].empty() && applied[j->q].back() + 1 != j->idx) badOrder++;
			if (applied[j->q].empty() && j->idx != 0) badOrder++;
			applied[j->q].push_back(j->idx);
			XeFence clr = kDone;
			if (j->clear) clr = XeFence{0, kClearSlot, 0, ++clearSeq};
			if (j->unbind) ++tlbSeq;
			const XeBindFence out = xeBindFenceNext(last[j->q], clr, j->unbind, tlbSeq, sig);
			last[j->q] = out;
			xeBjPublish(gTab, j->serial, out);
			for (Job **pp = &head; *pp; pp = &(*pp)->next)
				if (*pp == j) {
					*pp = j->next;
					if (tail == &j->next) tail = pp;
					break;
				}
			nApplied++;
		}
	};
	const int kCalls = 3000;
	for (int i = 0; i < kCalls; i++) {
		const int q = rand() % 3;
		Job *j = new Job();
		j->q = q;
		j->idx = made[q]++;
		j->clear = rand() % 2;
		j->unbind = rand() % 2;
		// in-fences: an exec seqno handed out (maybe not done), an earlier call's out-fence (any queue)
		if (rand() % 2) {
			const int k = rand() % 3;
			j->deps.push_back(XeFence{0, (uint32_t)k, 0, ++execSeq[k]});
		}
		if (!outs.empty() && rand() % 2) j->deps.push_back(outs[(size_t)rand() % outs.size()]);
		j->serial = xeBjAlloc(gTab, &next, sig);
		while (!j->serial) {   // ring full: the ioctl would wait; here completions run until a record is free
			step();
			worker();
			j->serial = xeBjAlloc(gTab, &next, sig);
		}
		*tail = j;
		tail = &j->next;
		all.push_back(j);
		outs.push_back(xeBindJobFence((uint32_t)q + 1, j->serial));
		outQ.push_back(q);
		prevSame.push_back(lastIdx[q]);
		lastIdx[q] = (int)outs.size() - 1;
		step();
		worker();
		// monotonic per queue (each sampled call against the one before it on its queue: transitively all of them),
		// and a signaled call's own parts complete
		for (int t = 0; t < 8; t++) {
			const size_t a = (size_t)rand() % outs.size();
			if (!sig(outs[a])) continue;
			if (prevSame[a] >= 0 && !sig(outs[(size_t)prevSame[a]])) badMono++;
			const XeBjRec &r = gTab[outs[a].seqno % kXeBjMax];
			if (r.serial == outs[a].seqno && !(sig(r.res.clear) && sig(r.res.tlb))) badOwn++;
		}
	}
	for (int guard = 0; head && guard < 100000; guard++) {   // everything completes in the end
		for (int k = 0; k < 3; k++) gExecDone[k] = execSeq[k];
		gClearDone = clearSeq;
		gExecDone[kClearSlot] = gClearDone;
		gTlbDone = tlbSeq;
		worker();
	}
	gClearDone = clearSeq;
	gExecDone[kClearSlot] = gClearDone;
	gTlbDone = tlbSeq;
	int unsignaled = 0;
	for (const XeFence &f : outs) unsignaled += !sig(f);
	CHECK(!head && nApplied == kCalls, "seed %u: applied %d of %d", seed, nApplied, kCalls);
	CHECK(!badDeps, "seed %u: %d calls applied before an in-fence had signaled", seed, badDeps);
	CHECK(!badOrder, "seed %u: %d calls applied out of their queue's order", seed, badOrder);
	CHECK(!badMono, "seed %u: %d out-fences signaled before an earlier one of their queue", seed, badMono);
	CHECK(!badOwn, "seed %u: %d out-fences signaled with their own clear or invalidation pending", seed, badOwn);
	CHECK(!unsignaled, "seed %u: %d out-fences unsignaled once everything completed", seed, unsignaled);
	for (Job *j : all) delete j;
}

static void testUfBusy() {
	// removes/splits: MAP, UNMAP over a pending VMA
	CHECK(xeUfBusy(true, false, false, 0x10000, 0x10000, 0x18000, 0x1000, true), "UNMAP over part of a pending VMA: EBUSY");
	CHECK(!xeUfBusy(true, false, false, 0x10000, 0x10000, 0x20000, 0x1000, true), "adjacent, not overlapping: no");
	CHECK(!xeUfBusy(true, false, false, 0x10000, 0x10000, 0x18000, 0x1000, false), "signaled user fence: no");
	CHECK(!xeUfBusy(false, false, false, 0x10000, 0x10000, 0x18000, 0x1000, true), "PREFETCH: never");
	CHECK(xeUfBusy(true, true, true, 0, 0, 0x18000, 0x1000, true), "UNMAP_ALL of the VMA's object: EBUSY");
	CHECK(!xeUfBusy(true, true, false, 0, 0, 0x18000, 0x1000, true), "UNMAP_ALL of another object: no");
	CHECK(!xeRangeOverlap(0, 0, 0, 0x1000), "an empty range overlaps nothing");
	CHECK(xeRangeOverlap(0xfffff000ull, 0x2000, 0x100000000ull, 0x1000), "overlap across 4 GB");
}

static void testSpan() {
	const XeBindSpan a = xeBindOpSpan(false, 0x10000, 0x10000), b = xeBindOpSpan(false, 0x1f000, 0x1000),
		c = xeBindOpSpan(false, 0x20000, 0x1000), all = xeBindOpSpan(true, 0, 0), none = xeBindOpSpan(false, 0x10000, 0);
	CHECK(xeBindSpanOverlap(a, b) && xeBindSpanOverlap(b, a), "overlapping ranges of two queues: ordered");
	CHECK(!xeBindSpanOverlap(a, c), "adjacent ranges: not ordered");
	CHECK(xeBindSpanOverlap(all, c) && xeBindSpanOverlap(a, all), "UNMAP_ALL meets every range");
	CHECK(!xeBindSpanOverlap(none, a) && !xeBindSpanOverlap(all, none), "an empty range meets nothing");
	const XeBindSpan top = xeBindOpSpan(false, 0xffffffffe000ull, 0x2000);
	CHECK(xeBindSpanOverlap(top, all) && top.hi == 0x1000000000000ull, "the top of the 48-bit space");
}

static void testOrphan() {
	const uint64_t lim = kXeUfOrphanNs;
	CHECK(xeUfOrphanAction(true, true, 100, 0, lim) == kXeUfWrite, "ready, process alive: written");
	CHECK(xeUfOrphanAction(true, false, 100, 0, lim) == kXeUfDropGone, "ready, process gone: dropped");
	CHECK(xeUfOrphanAction(false, false, 100, 0, lim) == kXeUfDropGone, "pending, process gone: dropped");
	CHECK(xeUfOrphanAction(false, true, lim - 1, 0, lim) == kXeUfKeep, "pending within the bound: kept");
	CHECK(xeUfOrphanAction(false, true, 5 + lim, 5, lim) == kXeUfDropLate, "pending at the bound: dropped");
	CHECK(xeUfOrphanAction(true, true, 5 + 2 * lim, 5, lim) == kXeUfWrite, "ready late, process alive: written");
	CHECK(lim >= 10000000000ull + xeTlbTimeoutNs(4096), "the bound covers a started job's limit and the TLB fence timeout");
}

int main() {
	testPath();
	testRecords();
	testPick();
	for (unsigned s = 1; s <= 20; s++) testModel(s);
	testUfBusy();
	testSpan();
	testOrphan();
	printf("bindq_test: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
