// SPDX-License-Identifier: MIT
// onednn_hello: oneDNN (oneAPI's deep-learning primitives, v3.14.2, compute/build-sycl.sh onednn) on the B580 through
// its SYCL GPU engine (DPC++ over our Level Zero; DESIGN-sycl.md section 7.5). oneDNN generates most kernels directly
// (nGEN, Xe2 binaries loaded with zeModuleCreate) and compiles its OpenCL C kernels at run time through the OpenCL
// device with the same UUID (NEO's OpenCL through the Khronos loader: OCL_ICD_FILENAMES=<l0igc>/lib/libigdrcl.dylib),
// so it runs over the IGC stage. Each step printed with PASS/FAIL:
//   1. a GPU engine on the SYCL queue's device and context (dnnl::sycl_interop::make_engine), its stream
//   2. eltwise ReLU forward on 1M floats (USM device memory through dnnl::sycl_interop::make_memory)
//   3. matmul f32 192x160 * 160x224 against a host reference (tolerance 1e-3 relative)
//   4. convolution f32 forward, 1x16x32x32 input, 32 3x3 filters, padding 1, against a host reference
// "build": step 1, then the three primitives created (their kernels compiled for the device), no execution, so it also
// runs against xemac's canned device (XEMAC_FAKE=1). Exit status 0 when every step passed.
//
// Build (compute/build-sycl.sh stage, when the onednn step was run): $SYCL_WORK/build/bin/clang++ -fsycl -O2
//   -I<oneDNN>/include -I<oneDNN build>/include onednn_hello.cpp -L<oneDNN build>/src -ldnnl -o onednn_hello
// Run: export ONEAPI_DEVICE_SELECTOR=level_zero:gpu DYLD_LIBRARY_PATH=<sycl stage>/lib:<l0igc>/lib
//   OCL_ICD_FILENAMES=<l0igc>/lib/libigdrcl.dylib; ./onednn_hello
#include <oneapi/dnnl/dnnl.hpp>
#include <oneapi/dnnl/dnnl_sycl.hpp>

#include <sycl/sycl.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace dnnl;
using tag = memory::format_tag;
using dt = memory::data_type;

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

static memory usm_memory(const memory::desc &md, const engine &eng, sycl::queue &q, const std::vector<float> *init)
{
	memory m = sycl_interop::make_memory(md, eng, sycl_interop::memory_kind::usm);
	if (init)
		q.memcpy(m.get_data_handle(), init->data(), init->size() * sizeof(float)).wait();
	return m;
}

static std::vector<float> to_host(const memory &m, sycl::queue &q, size_t n)
{
	std::vector<float> h(n);
	q.memcpy(h.data(), m.get_data_handle(), n * sizeof(float)).wait();
	return h;
}

static bool close(const std::vector<float> &a, const std::vector<float> &b, const char *what)
{
	for (size_t i = 0; i < a.size(); i++) {
		const float tol = 1e-3f * std::max(1.0f, std::fabs(b[i]));
		if (!(std::fabs(a[i] - b[i]) <= tol)) {
			printf("  %s: [%zu] = %g, expected %g\n", what, i, a[i], b[i]);
			return false;
		}
	}
	return true;
}

static float val(size_t i, int k) { return (float)((int)((i * 2654435761u + k) % 17) - 8) / 8.0f; }

int main(int argc, char **argv)
{
	const bool build_only = argc > 1 && !strcmp(argv[1], "build");
	try {
		sycl::queue q{sycl::gpu_selector_v};
		announce("engine");
		engine eng = sycl_interop::make_engine(q.get_device(), q.get_context());
		stream strm = sycl_interop::make_stream(eng, q);
		printf("  oneDNN %d.%d.%d, engine kind %s on %s\n", dnnl_version()->major, dnnl_version()->minor,
		       dnnl_version()->patch, eng.get_kind() == engine::kind::gpu ? "gpu" : "other",
		       q.get_device().get_info<sycl::info::device::name>().c_str());
		step("oneDNN GPU engine on the SYCL device", eng.get_kind() == engine::kind::gpu);

		// ReLU
		const memory::dim n = 1 << 20;
		memory::desc rmd({n}, dt::f32, tag::a);
		announce("eltwise ReLU");
		eltwise_forward::primitive_desc relu_pd(eng, prop_kind::forward_inference, algorithm::eltwise_relu, rmd, rmd,
							0.0f);
		eltwise_forward relu(relu_pd);
		if (build_only) {
			step("eltwise ReLU created", true);
		} else {
			std::vector<float> src(n);
			for (size_t i = 0; i < (size_t)n; i++)
				src[i] = val(i, 1);
			memory s = usm_memory(rmd, eng, q, &src), d = usm_memory(rmd, eng, q, nullptr);
			relu.execute(strm, {{DNNL_ARG_SRC, s}, {DNNL_ARG_DST, d}});
			strm.wait();
			std::vector<float> e(n);
			for (size_t i = 0; i < (size_t)n; i++)
				e[i] = src[i] > 0 ? src[i] : 0;
			step("eltwise ReLU (1M floats)", close(to_host(d, q, n), e, "relu"));
		}

		// matmul
		const memory::dim M = 192, K = 160, N = 224;
		memory::desc amd({M, K}, dt::f32, tag::ab), bmd({K, N}, dt::f32, tag::ab), cmd({M, N}, dt::f32, tag::ab);
		announce("matmul");
		matmul::primitive_desc mm_pd(eng, amd, bmd, cmd);
		matmul mm(mm_pd);
		printf("  matmul implementation: %s\n", mm_pd.impl_info_str());
		if (build_only) {
			step("matmul created", true);
		} else {
			std::vector<float> a(M * K), b(K * N), e(M * N, 0.0f);
			for (size_t i = 0; i < a.size(); i++)
				a[i] = val(i, 2);
			for (size_t i = 0; i < b.size(); i++)
				b[i] = val(i, 3);
			for (memory::dim i = 0; i < M; i++)
				for (memory::dim k = 0; k < K; k++)
					for (memory::dim j = 0; j < N; j++)
						e[i * N + j] += a[i * K + k] * b[k * N + j];
			memory ma = usm_memory(amd, eng, q, &a), mb = usm_memory(bmd, eng, q, &b), mc = usm_memory(cmd, eng, q, nullptr);
			mm.execute(strm, {{DNNL_ARG_SRC, ma}, {DNNL_ARG_WEIGHTS, mb}, {DNNL_ARG_DST, mc}});
			strm.wait();
			step("matmul f32 192x160x224", close(to_host(mc, q, M * N), e, "matmul"));
		}

		// convolution
		const memory::dim C = 16, H = 32, W = 32, OC = 32, R = 3;
		memory::desc xmd({1, C, H, W}, dt::f32, tag::nchw), wmd({OC, C, R, R}, dt::f32, tag::oihw),
			ymd({1, OC, H, W}, dt::f32, tag::nchw);
		announce("convolution");
		convolution_forward::primitive_desc cv_pd(eng, prop_kind::forward_inference, algorithm::convolution_direct, xmd,
							  wmd, ymd, {1, 1}, {1, 1}, {1, 1});
		convolution_forward cv(cv_pd);
		printf("  convolution implementation: %s\n", cv_pd.impl_info_str());
		if (build_only) {
			step("convolution created", true);
		} else {
			std::vector<float> x(C * H * W), w(OC * C * R * R), e(OC * H * W, 0.0f);
			for (size_t i = 0; i < x.size(); i++)
				x[i] = val(i, 4);
			for (size_t i = 0; i < w.size(); i++)
				w[i] = val(i, 5);
			for (memory::dim o = 0; o < OC; o++)
				for (memory::dim y = 0; y < H; y++)
					for (memory::dim xx = 0; xx < W; xx++) {
						float acc = 0;
						for (memory::dim c = 0; c < C; c++)
							for (memory::dim r = 0; r < R; r++)
								for (memory::dim s = 0; s < R; s++) {
									const memory::dim iy = y + r - 1, ix = xx + s - 1;
									if (iy < 0 || iy >= H || ix < 0 || ix >= W)
										continue;
									acc += x[(c * H + iy) * W + ix] * w[((o * C + c) * R + r) * R + s];
								}
						e[(o * H + y) * W + xx] = acc;
					}
			memory mx = usm_memory(xmd, eng, q, &x), mw = usm_memory(wmd, eng, q, &w), my = usm_memory(ymd, eng, q, nullptr);
			cv.execute(strm, {{DNNL_ARG_SRC, mx}, {DNNL_ARG_WEIGHTS, mw}, {DNNL_ARG_DST, my}});
			strm.wait();
			step("convolution f32 3x3 (1x16x32x32 -> 32 channels)", close(to_host(my, q, OC * H * W), e, "conv"));
		}
	} catch (const dnnl::error &e) {
		printf("FAIL oneDNN error: %s (status %d)\n", e.what(), (int)e.status);
		failures++;
	} catch (const sycl::exception &e) {
		printf("FAIL SYCL exception: %s\n", e.what());
		failures++;
	}
	printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
