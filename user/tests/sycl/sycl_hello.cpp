// SPDX-License-Identifier: MIT
// sycl_hello: SYCL 2020 on a Level Zero GPU through Intel's DPC++ runtime (libsycl, the Unified Runtime's Level Zero
// adapters) over our Level Zero (compute-runtime/NEO on xemac). Each step printed with PASS/FAIL:
//   1. platforms and devices; the queue's device: backend, compute units, sub-group sizes, memory, USM aspects; and
//      Intel's device queries the runtime answers from Level Zero (the max clock, memory clock and bus width, and the
//      free memory: the core's usable-memory extension when NEO offers it, else sysman's zesMemoryGetState):
//      "PASS free memory" when it is known and within the device's memory, "SKIP free memory" without
//      ext_intel_free_memory; then the queries the runtime answers only through Level Zero sysman (zesInit, the card
//      power domain, the GPU frequency domain's throttle reasons): "PASS sysman" when the device offers
//      ext_intel_power_limits and both answer, "SKIP sysman" without it (a driver whose zesInit fails)
//   2. vector add of 1M floats through buffers and accessors
//   3. sycl::reduction (plus, 64-bit) of 1..262144
//   4. USM shared: a kernel writes, the host reads
//   5. USM device: host -> device, device -> device and device -> host memcpy around a kernel
//   6. fill and memset on device memory
// "build": step 1, then every kernel of the program built for the device (get_kernel_bundle<executable>), no GPU work
// (also against xemac's canned device, XEMAC_FAKE=1, where nothing executes).
// "vector", "reduction", "usm_shared", "usm_device", "fill" (one or more): step 1, then only those steps. Against the
// canned device "reduction" is a host-side check of the reduction's submission (13 Nov: libsycl and the program
// disagreed on how a type_erased_cgfo_ty is passed and the submission called a null pointer; intel-llvm patch 0006):
// it must get as far as waiting for the GPU, which fake mode never answers.
// Exit status 0 when every step passed.
//
// Build (compute/build-sycl.sh stage): $SYCL_WORK/build/bin/clang++ -fsycl -fsycl-targets=spir64_gen,spir64
//   -Xsycl-target-backend=spir64_gen "-device bmg" -O2 sycl_hello.cpp -o sycl_hello  (spir64 only: sycl_hello_jit)
// Run: export ONEAPI_DEVICE_SELECTOR=level_zero:gpu DYLD_LIBRARY_PATH=<sycl stage>/lib:<l0 stage>/lib; ./sycl_hello
#include <sycl/sycl.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

static int failures;

static void step(const char *name, bool ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	fflush(stdout);
	if (!ok)
		failures++;
}

static const char *backend_name(sycl::backend b)
{
	switch (b) {
	case sycl::backend::ext_oneapi_level_zero: return "level_zero";
	case sycl::backend::opencl: return "opencl";
	default: return "other";
	}
}

static bool device_query(const sycl::queue &q)
{
	for (const auto &p : sycl::platform::get_platforms()) {
		printf("platform: %s (%s, %s)\n", p.get_info<sycl::info::platform::name>().c_str(),
		       p.get_info<sycl::info::platform::version>().c_str(), backend_name(p.get_backend()));
		for (const auto &d : p.get_devices())
			printf("  device: %s (%s)\n", d.get_info<sycl::info::device::name>().c_str(), d.is_gpu() ? "gpu" : "other");
	}
	const sycl::device d = q.get_device();
	printf("queue device: %s\n", d.get_info<sycl::info::device::name>().c_str());
	printf("  vendor %s, driver %s, backend %s\n", d.get_info<sycl::info::device::vendor>().c_str(),
	       d.get_info<sycl::info::device::driver_version>().c_str(), backend_name(d.get_backend()));
	const uint64_t global = d.get_info<sycl::info::device::global_mem_size>();
	printf("  compute units %u, max work-group %zu, global memory %llu MB, local memory %llu KB\n",
	       d.get_info<sycl::info::device::max_compute_units>(), d.get_info<sycl::info::device::max_work_group_size>(),
	       (unsigned long long)(global >> 20),
	       (unsigned long long)(d.get_info<sycl::info::device::local_mem_size>() >> 10));
	printf("  sub-group sizes:");
	for (size_t s : d.get_info<sycl::info::device::sub_group_sizes>())
		printf(" %zu", s);
	printf("\n  usm: device %d, host %d, shared %d; fp64 %d\n", d.has(sycl::aspect::usm_device_allocations),
	       d.has(sycl::aspect::usm_host_allocations), d.has(sycl::aspect::usm_shared_allocations),
	       d.has(sycl::aspect::fp64));
	return d.is_gpu() && d.get_backend() == sycl::backend::ext_oneapi_level_zero;
}

// Intel's device queries (sycl::ext::intel::info::device). Returns -1 (skip) when the device does not offer the
// free-memory query.
static int intel_query(const sycl::queue &q)
{
	const sycl::device d = q.get_device();
	printf("  max clock %u MHz", d.get_info<sycl::info::device::max_clock_frequency>());
	if (d.has(sycl::aspect::ext_intel_memory_clock_rate))
		printf(", memory clock %u MHz", d.get_info<sycl::ext::intel::info::device::memory_clock_rate>());
	if (d.has(sycl::aspect::ext_intel_memory_bus_width))
		printf(", memory bus %u bits", d.get_info<sycl::ext::intel::info::device::memory_bus_width>());
	if (!d.has(sycl::aspect::ext_intel_free_memory)) {
		printf("; no free-memory query (ext_intel_free_memory)\n");
		return -1;
	}
	const uint64_t free = d.get_info<sycl::ext::intel::info::device::free_memory>();
	const uint64_t global = d.get_info<sycl::info::device::global_mem_size>();
	printf("; free memory %llu MB of %llu MB\n", (unsigned long long)(free >> 20), (unsigned long long)(global >> 20));
	return free > 0 && free <= global;
}

// The runtime asks these of sysman only (urDeviceGetInfo: zesDeviceGetCardPowerDomain + zesPowerGetProperties, and
// zesFrequencyGetState of the GPU domain). Returns -1 (skip) without ext_intel_power_limits.
static int sysman_query(const sycl::queue &q)
{
	const sycl::device d = q.get_device();
	if (!d.has(sycl::aspect::ext_intel_power_limits)) {
		printf("  no sysman power limits (ext_intel_power_limits)\n");
		return -1;
	}
	try {
		const int32_t mn = d.get_info<sycl::ext::intel::info::device::min_power_limit>();
		const int32_t mx = d.get_info<sycl::ext::intel::info::device::max_power_limit>();
		printf("  sysman: card power limits min %d, max %d mW", mn, mx);
		if (d.has(sycl::aspect::ext_intel_current_clock_throttle_reasons)) {
			const auto r = d.get_info<sycl::ext::intel::info::device::current_clock_throttle_reasons>();
			printf("; %zu throttle reason(s)", r.size());
		}
		printf("\n");
		return 1;
	} catch (const sycl::exception &e) {
		printf("  sysman query failed: %s\n", e.what());
		return 0;
	}
}

static bool vector_add(sycl::queue &q)
{
	constexpr size_t n = 1 << 20;
	std::vector<float> a(n), b(n), c(n, -1.0f);
	for (size_t i = 0; i < n; i++) {
		a[i] = (float)i;
		b[i] = 2.0f * (float)i;
	}
	{
		sycl::buffer<float> ba(a.data(), sycl::range<1>(n)), bb(b.data(), sycl::range<1>(n)),
			bc(c.data(), sycl::range<1>(n));
		q.submit([&](sycl::handler &h) {
			sycl::accessor x(ba, h, sycl::read_only), y(bb, h, sycl::read_only);
			sycl::accessor z(bc, h, sycl::write_only, sycl::no_init);
			h.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) { z[i] = x[i] + y[i]; });
		});
	}
	for (size_t i = 0; i < n; i++)
		if (c[i] != 3.0f * (float)i)
			return false;
	return true;
}

static bool reduction(sycl::queue &q)
{
	constexpr size_t n = 262144;
	uint64_t sum = 0;
	{
		sycl::buffer<uint64_t> bs(&sum, 1);
		q.submit([&](sycl::handler &h) {
			auto r = sycl::reduction(bs, h, sycl::plus<uint64_t>());
			h.parallel_for(sycl::range<1>(n), r, [=](sycl::id<1> i, auto &acc) { acc += (uint64_t)i[0] + 1; });
		});
	}
	return sum == (uint64_t)n * (n + 1) / 2;
}

static bool usm_shared(sycl::queue &q)
{
	constexpr size_t n = 65536;
	uint32_t *p = sycl::malloc_shared<uint32_t>(n, q);
	if (!p)
		return false;
	q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) { p[i] = (uint32_t)i[0] * 7 + 3; }).wait();
	bool ok = true;
	for (size_t i = 0; i < n && ok; i++)
		ok = p[i] == (uint32_t)i * 7 + 3;
	sycl::free(p, q);
	return ok;
}

static bool usm_device(sycl::queue &q)
{
	constexpr size_t n = 1 << 18;
	std::vector<int32_t> host(n), back(n, 0);
	std::iota(host.begin(), host.end(), 0);
	int32_t *d0 = sycl::malloc_device<int32_t>(n, q), *d1 = sycl::malloc_device<int32_t>(n, q);
	if (!d0 || !d1)
		return false;
	q.memcpy(d0, host.data(), n * sizeof(int32_t)).wait();
	q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) { d0[i] = d0[i] * 2 + 1; }).wait();
	q.memcpy(d1, d0, n * sizeof(int32_t)).wait();
	q.memcpy(back.data(), d1, n * sizeof(int32_t)).wait();
	sycl::free(d0, q);
	sycl::free(d1, q);
	for (size_t i = 0; i < n; i++)
		if (back[i] != (int32_t)i * 2 + 1)
			return false;
	return true;
}

static bool fill_memset(sycl::queue &q)
{
	constexpr size_t n = 1 << 16;
	uint32_t *d = sycl::malloc_device<uint32_t>(n, q);
	uint8_t *m = sycl::malloc_device<uint8_t>(n, q);
	if (!d || !m)
		return false;
	std::vector<uint32_t> back(n, 0);
	std::vector<uint8_t> mback(n, 0);
	q.fill(d, 0xdeadbeefu, n).wait();
	q.memset(m, 0x5a, n).wait();
	q.memcpy(back.data(), d, n * sizeof(uint32_t)).wait();
	q.memcpy(mback.data(), m, n).wait();
	sycl::free(d, q);
	sycl::free(m, q);
	for (size_t i = 0; i < n; i++)
		if (back[i] != 0xdeadbeefu || mback[i] != 0x5a)
			return false;
	return true;
}

int main(int argc, char **argv)
{
	const bool build_only = argc > 1 && !strcmp(argv[1], "build");
	try {
		sycl::queue q{sycl::gpu_selector_v};
		step("device query (a Level Zero GPU)", device_query(q));
		const int fm = intel_query(q);
		if (fm < 0)
			printf("SKIP free memory (no ext_intel_free_memory)\n");
		else
			step("free memory (within the device's memory)", fm == 1);
		const int sm = sysman_query(q);
		if (sm < 0)
			printf("SKIP sysman (no ext_intel_power_limits)\n");
		else
			step("sysman (power limits and throttle reasons through zesInit)", sm == 1);
		static const struct {
			const char *arg, *name;
			bool (*run)(sycl::queue &);
		} steps[] = {
			{"vector", "vector add (buffers, accessors)", vector_add},
			{"reduction", "reduction (plus, 64-bit)", reduction},
			{"usm_shared", "USM shared (kernel writes, host reads)", usm_shared},
			{"usm_device", "USM device (copies around a kernel)", usm_device},
			{"fill", "fill and memset", fill_memset},
		};
		if (build_only) {
			auto bundle = sycl::get_kernel_bundle<sycl::bundle_state::executable>(q.get_context(), {q.get_device()});
			const size_t kernels = bundle.get_kernel_ids().size();
			printf("%zu kernels\n", kernels);
			step("kernel build (every kernel of the program for the device)", kernels > 0);
		} else {
			for (const auto &st : steps) {
				bool chosen = argc < 2;
				for (int a = 1; a < argc && !chosen; a++)
					chosen = !strcmp(argv[a], st.arg);
				if (chosen) {
					printf("%s ...\n", st.name);
					fflush(stdout);
					step(st.name, st.run(q));
				}
			}
		}
	} catch (const sycl::exception &e) {
		printf("FAIL exception: %s\n", e.what());
		failures++;
	}
	printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
