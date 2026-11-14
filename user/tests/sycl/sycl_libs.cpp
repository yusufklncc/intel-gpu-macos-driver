// SPDX-License-Identifier: MIT
// sycl_libs: what sits on top of the SYCL runtime (compute/build-sycl.sh, DESIGN-sycl.md section 6) on a Level Zero GPU
// through Intel's DPC++ runtime over our Level Zero (compute-runtime/NEO on xemac). Each step printed with PASS/FAIL:
//   rtc  run-time compilation (sycl_ext_oneapi_kernel_compiler): device::ext_oneapi_can_build for SPIR-V, OpenCL C
//        and SYCL; then one kernel from each, built (syclex::build) and run on USM:
//          spirv   a SPIR-V module read from <exe dir>/../share/rtc_kernels.spv (OpenCL C compiled offline by the
//                  DPC++ clang + llvm-spirv at stage time): the driver's IGC compiles it
//          opencl  OpenCL C source: libsycl hands it to ocloc (libocloc.dylib from the IGC stage, intel-llvm 0007)
//          sycl    SYCL source (a free-function kernel): libsycl-jit.dylib compiles it in the process (clang, the
//                  SYCL headers and libdevice embedded in it; intel-llvm 0008), then IGC. The host C++ headers
//                  (libc++, the C library) come from a macOS SDK: the compiler's default one, or the directory in
//                  SYCL_RTC_SYSROOT (passed as -isysroot; the stage carries the headers this kernel includes)
//   dpl  oneDPL (oneAPI's parallel algorithms) with a device policy on the queue, USM device memory: sort (int32),
//        sort with a comparator (descending floats), reduce (int64), transform_reduce (a dot product), inclusive_scan,
//        count_if, find_if, copy_if
// "rtc-build" / "dpl-build": the same without GPU work (the SPIR-V/OpenCL/SYCL bundles built and their kernels
// found; every oneDPL kernel of the program built for the device), so they also run against xemac's canned device
// (XEMAC_FAKE=1). No argument: rtc and dpl. Exit status 0 when every step passed (a SKIP is not a failure).
//
// Build (compute/build-sycl.sh stage): $SYCL_WORK/build/bin/clang++ -fsycl -fsycl-targets=spir64_gen,spir64
//   -Xsycl-target-backend=spir64_gen "-device bmg" -O2 -I<oneDPL>/include sycl_libs.cpp -o sycl_libs
// Run: export ONEAPI_DEVICE_SELECTOR=level_zero:gpu DYLD_LIBRARY_PATH=<sycl stage>/lib:<l0 stage>/lib; ./sycl_libs dpl
#include <oneapi/dpl/algorithm>
#include <oneapi/dpl/execution>
#include <oneapi/dpl/numeric>

#include <sycl/sycl.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace syclex = sycl::ext::oneapi::experimental;

static int failures;

static void step(const char *name, bool ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	fflush(stdout);
	if (!ok)
		failures++;
}

static void announce(const char *name)
{
	printf("%s ...\n", name);
	fflush(stdout);
}

// ---- rtc ----

static std::string exe_dir;

static const char *opencl_source = R"CL(
__kernel void rtc_cl_scale(__global const int *in, __global int *out, int k)
{
	size_t i = get_global_id(0);
	out[i] = in[i] * k + 1;
}
)CL";

static const char *sycl_source = R"SYCL(
#include <sycl/sycl.hpp>
namespace syclext = sycl::ext::oneapi;
namespace syclexp = sycl::ext::oneapi::experimental;

extern "C" SYCL_EXT_ONEAPI_FUNCTION_PROPERTY((syclexp::nd_range_kernel<1>))
void rtc_sycl_iota(int start, int *out)
{
	size_t id = syclext::this_work_item::get_nd_item<1>().get_global_linear_id();
	out[id] = start + static_cast<int>(id) * 2;
}
)SYCL";

static std::vector<std::byte> read_file(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return {};
	std::vector<char> c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	std::vector<std::byte> b(c.size());
	memcpy(b.data(), c.data(), c.size());
	return b;
}

static bool can_build(sycl::device d)
{
	const bool spv = d.ext_oneapi_can_build(syclex::source_language::spirv);
	const bool cl = d.ext_oneapi_can_build(syclex::source_language::opencl);
	const bool sy = d.ext_oneapi_can_build(syclex::source_language::sycl);
	printf("  can_build: spirv %d, opencl %d, sycl %d; can_compile sycl %d\n", spv, cl, sy,
	       d.ext_oneapi_can_compile(syclex::source_language::sycl));
	return spv && cl && sy;
}

constexpr size_t rtc_n = 4096;

// in[i] = i; a kernel taking (in, out, k) must give out[i] = i * k + 1
static bool run_scale(sycl::queue &q, sycl::kernel k, const char *what)
{
	int32_t *in = sycl::malloc_device<int32_t>(rtc_n, q), *out = sycl::malloc_device<int32_t>(rtc_n, q);
	std::vector<int32_t> h(rtc_n), back(rtc_n, -1);
	for (size_t i = 0; i < rtc_n; i++)
		h[i] = (int32_t)i;
	q.memcpy(in, h.data(), rtc_n * sizeof(int32_t)).wait();
	q.submit([&](sycl::handler &cgh) {
		cgh.set_args(in, out, 5);
		cgh.parallel_for(sycl::range<1>(rtc_n), k);
	}).wait();
	q.memcpy(back.data(), out, rtc_n * sizeof(int32_t)).wait();
	sycl::free(in, q);
	sycl::free(out, q);
	for (size_t i = 0; i < rtc_n; i++)
		if (back[i] != (int32_t)i * 5 + 1) {
			printf("  %s: out[%zu] = %d, expected %d\n", what, i, back[i], (int32_t)i * 5 + 1);
			return false;
		}
	return true;
}

static void rtc(sycl::queue &q, bool run)
{
	sycl::device d = q.get_device();
	const sycl::context ctx = q.get_context();
	announce("rtc: can_build");
	step("rtc can_build (SPIR-V, OpenCL C and SYCL)", can_build(d));

	announce("rtc: SPIR-V");
	try {
		const std::string path = exe_dir + "/../share/rtc_kernels.spv";
		const std::vector<std::byte> spv = read_file(path);
		if (spv.empty()) {
			printf("  cannot read %s\n", path.c_str());
			step("rtc SPIR-V", false);
		} else {
			auto src = syclex::create_kernel_bundle_from_source(ctx, syclex::source_language::spirv, spv);
			auto exe = syclex::build(src);
			sycl::kernel k = exe.ext_oneapi_get_kernel("rtc_spv_scale");
			printf("  %zu bytes of SPIR-V, kernel rtc_spv_scale\n", spv.size());
			step(run ? "rtc SPIR-V (built and run)" : "rtc SPIR-V (built)", !run || run_scale(q, k, "spirv"));
		}
	} catch (const sycl::exception &e) {
		printf("  exception: %s\n", e.what());
		step("rtc SPIR-V", false);
	}

	announce("rtc: OpenCL C");
	try {
		auto src = syclex::create_kernel_bundle_from_source(ctx, syclex::source_language::opencl, opencl_source);
		std::string log;
		auto exe = syclex::build(src, syclex::properties{syclex::save_log{&log}});
		sycl::kernel k = exe.ext_oneapi_get_kernel("rtc_cl_scale");
		step(run ? "rtc OpenCL C (built by ocloc and run)" : "rtc OpenCL C (built by ocloc)",
		     !run || run_scale(q, k, "opencl"));
	} catch (const sycl::exception &e) {
		printf("  exception: %s\n", e.what());
		step("rtc OpenCL C", false);
	}

	announce("rtc: SYCL");
	std::string sycl_log;
	try {
		std::vector<std::string> opts;
		if (const char *sr = getenv("SYCL_RTC_SYSROOT"); sr && *sr) {
			opts.push_back("-isysroot");
			opts.push_back(sr);
			printf("  -isysroot %s\n", sr);
		}
		auto src = syclex::create_kernel_bundle_from_source(ctx, syclex::source_language::sycl, sycl_source);
		auto exe = syclex::build(src, syclex::properties{syclex::build_options{opts}, syclex::save_log{&sycl_log}});
		sycl::kernel k = exe.ext_oneapi_get_kernel("rtc_sycl_iota");
		bool ok = true;
		if (run) {
			constexpr size_t n = 1024;
			int32_t *out = sycl::malloc_shared<int32_t>(n, q);
			q.submit([&](sycl::handler &cgh) {
				cgh.set_args(7, out);
				cgh.parallel_for(sycl::nd_range<1>{{n}, {64}}, k);
			}).wait();
			for (size_t i = 0; i < n && ok; i++)
				if (out[i] != 7 + 2 * (int32_t)i) {
					printf("  sycl: out[%zu] = %d, expected %d\n", i, out[i], 7 + 2 * (int32_t)i);
					ok = false;
				}
			sycl::free(out, q);
		}
		step(run ? "rtc SYCL (built by sycl-jit and run)" : "rtc SYCL (built by sycl-jit)", ok);
	} catch (const sycl::exception &e) {
		std::string w = e.what();
		if (w.size() > 3000)
			w = w.substr(0, 3000) + " ...";
		printf("  exception: %s\n", w.c_str());
		if (!sycl_log.empty())
			printf("  build log:\n%.3000s\n", sycl_log.c_str());
		step("rtc SYCL", false);
	}
}

// ---- dpl ----

template <typename T> static std::vector<T> to_host(sycl::queue &q, const T *d, size_t n)
{
	std::vector<T> h(n);
	q.memcpy(h.data(), d, n * sizeof(T)).wait();
	return h;
}

static void dpl_run(sycl::queue &q)
{
	auto pol = oneapi::dpl::execution::make_device_policy(q);
	constexpr size_t n = 1 << 20;
	std::vector<int32_t> hi(n);
	std::vector<float> hf(n);
	uint32_t x = 12345;
	for (size_t i = 0; i < n; i++) {
		x = x * 1664525u + 1013904223u;
		hi[i] = (int32_t)(x >> 1) - (1 << 30);
		hf[i] = (float)(x >> 8) / 65536.0f;
	}
	int32_t *di = sycl::malloc_device<int32_t>(n, q), *dj = sycl::malloc_device<int32_t>(n, q);
	float *df = sycl::malloc_device<float>(n, q), *dg = sycl::malloc_device<float>(n, q);
	int64_t *dl = sycl::malloc_device<int64_t>(n, q);
	if (!di || !dj || !df || !dg || !dl) {
		step("dpl USM allocation", false);
		return;
	}
	auto guarded = [&](const char *name, auto fn) {
		announce(name);
		try {
			step(name, fn());
		} catch (const std::exception &e) {
			printf("  exception: %s\n", e.what());
			step(name, false);
		}
	};
	guarded("dpl sort (int32, 1M)", [&] {
		q.memcpy(di, hi.data(), n * sizeof(int32_t)).wait();
		oneapi::dpl::sort(pol, di, di + n);
		std::vector<int32_t> r = to_host(q, di, n), e = hi;
		std::sort(e.begin(), e.end());
		return r == e;
	});
	guarded("dpl sort with a comparator (float, descending)", [&] {
		q.memcpy(df, hf.data(), n * sizeof(float)).wait();
		oneapi::dpl::sort(pol, df, df + n, std::greater<float>());
		std::vector<float> r = to_host(q, df, n), e = hf;
		std::sort(e.begin(), e.end(), std::greater<float>());
		return r == e;
	});
	guarded("dpl reduce (int64)", [&] {
		std::vector<int64_t> h(n);
		for (size_t i = 0; i < n; i++)
			h[i] = (int64_t)hi[i];
		q.memcpy(dl, h.data(), n * sizeof(int64_t)).wait();
		const int64_t r = oneapi::dpl::reduce(pol, dl, dl + n, (int64_t)0);
		int64_t e = 0;
		for (int64_t v : h)
			e += v;
		printf("  sum %lld (expected %lld)\n", (long long)r, (long long)e);
		return r == e;
	});
	guarded("dpl transform_reduce (dot product of small integers)", [&] {
		std::vector<float> a(n), b(n);
		for (size_t i = 0; i < n; i++) {
			a[i] = (float)(i % 7);
			b[i] = (float)(i % 5);
		}
		q.memcpy(df, a.data(), n * sizeof(float)).wait();
		q.memcpy(dg, b.data(), n * sizeof(float)).wait();
		// every partial sum is an integer below 2^24 per work-item block; compare in double on the host
		const double r = oneapi::dpl::transform_reduce(pol, df, df + n, dg, 0.0);
		double e = 0;
		for (size_t i = 0; i < n; i++)
			e += (double)a[i] * b[i];
		printf("  dot %.0f (expected %.0f)\n", r, e);
		return r == e;
	});
	guarded("dpl inclusive_scan (int32)", [&] {
		std::vector<int32_t> h(n);
		for (size_t i = 0; i < n; i++)
			h[i] = (int32_t)(i % 13) - 6;
		q.memcpy(di, h.data(), n * sizeof(int32_t)).wait();
		oneapi::dpl::inclusive_scan(pol, di, di + n, dj);
		std::vector<int32_t> r = to_host(q, dj, n), e(n);
		std::inclusive_scan(h.begin(), h.end(), e.begin());
		return r == e;
	});
	guarded("dpl count_if and find_if", [&] {
		q.memcpy(di, hi.data(), n * sizeof(int32_t)).wait();
		const auto c = oneapi::dpl::count_if(pol, di, di + n, [](int32_t v) { return v % 3 == 0; });
		const auto e = std::count_if(hi.begin(), hi.end(), [](int32_t v) { return v % 3 == 0; });
		const int32_t target = hi[n / 2 + 17];
		const auto f = oneapi::dpl::find_if(pol, di, di + n, [=](int32_t v) { return v == target; });
		const auto ef = std::find(hi.begin(), hi.end(), target);
		printf("  count %lld (expected %lld), found at %lld (expected %lld)\n", (long long)c, (long long)e,
		       (long long)(f - di), (long long)(ef - hi.begin()));
		return c == e && f - di == ef - hi.begin();
	});
	guarded("dpl copy_if (stable, positives)", [&] {
		q.memcpy(di, hi.data(), n * sizeof(int32_t)).wait();
		int32_t *end = oneapi::dpl::copy_if(pol, di, di + n, dj, [](int32_t v) { return v > 0; });
		const size_t m = (size_t)(end - dj);
		std::vector<int32_t> r = to_host(q, dj, m), e;
		std::copy_if(hi.begin(), hi.end(), std::back_inserter(e), [](int32_t v) { return v > 0; });
		printf("  %zu copied (expected %zu)\n", m, e.size());
		return r == e;
	});
	sycl::free(di, q);
	sycl::free(dj, q);
	sycl::free(df, q);
	sycl::free(dg, q);
	sycl::free(dl, q);
}

static void dpl_build(sycl::queue &q)
{
	announce("dpl kernel build");
	auto bundle = sycl::get_kernel_bundle<sycl::bundle_state::executable>(q.get_context(), {q.get_device()});
	const size_t kernels = bundle.get_kernel_ids().size();
	printf("  %zu kernels (oneDPL's, instantiated by this program)\n", kernels);
	step("dpl kernel build (every kernel of the program for the device)", kernels > 0);
}

int main(int argc, char **argv)
{
	exe_dir = argv[0];
	const size_t slash = exe_dir.rfind('/');
	exe_dir = slash == std::string::npos ? "." : exe_dir.substr(0, slash);
	auto want = [&](const char *a) {
		if (argc < 2)
			return strcmp(a, "rtc") == 0 || strcmp(a, "dpl") == 0;
		for (int i = 1; i < argc; i++)
			if (!strcmp(argv[i], a))
				return true;
		return false;
	};
	try {
		sycl::queue q{sycl::gpu_selector_v};
		const sycl::device d = q.get_device();
		printf("queue device: %s (%s)\n", d.get_info<sycl::info::device::name>().c_str(),
		       d.get_backend() == sycl::backend::ext_oneapi_level_zero ? "level_zero" : "other");
		if (want("rtc") || want("rtc-build"))
			rtc(q, want("rtc"));
		if (want("dpl-build"))
			dpl_build(q);
		if (want("dpl"))
			dpl_run(q);
	} catch (const sycl::exception &e) {
		printf("FAIL exception: %s\n", e.what());
		failures++;
	}
	printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
