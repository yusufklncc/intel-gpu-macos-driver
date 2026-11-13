// SPDX-License-Identifier: MIT
// cl_hello: Intel's OpenCL driver (compute-runtime/NEO, libigdrcl.dylib) on macOS over IntelXe.kext, through the
// Khronos OpenCL ICD loader (libOpenCL.1.dylib, not Apple's OpenCL.framework). Steps, PASS/FAIL each:
//   1. platforms and devices: names, versions, compute units, memory
//   2. a context and an in-order queue; a buffer written and read back (blocking), compared
//   3. "kernel <file.bin> [name]": a native Battlemage binary (clCreateProgramWithBinary; fill_ids by default:
//      out[i] = i * 3 + 1 for 4096 items), run and checked
//      "source <file.cl> [name]": the same kernel from OpenCL C (clCreateProgramWithSource + clBuildProgram: IGC's
//      front end libigdfcl and back end libigc at run time), run and checked
//      "source-build <file.cl> [name]": only the build (program binary size, kernel created): no GPU work, so it also
//      runs against xemac's canned device (XEMAC_FAKE=1)
//   "lifetime": instead of 2-3, the object lifetimes only: a small buffer (from the context's small-buffers pool) and
//      a large one created and released, then the queue and the context: no GPU work, so it also runs with
//      XEMAC_FAKE=1. NEO aborts in clReleaseContext (reference_tracked_object.h:137) without patch 0012.
// Exit status 0 when every step that ran passed.
//
// Build (the build Mac; L0 = the prefix compute/build-neo.sh installs to, with the ICD loader and OpenCL headers):
//   clang -arch x86_64 -O1 -I$L0/include -o cl_hello cl_hello.c -L$L0/lib -lOpenCL -Wl,-rpath,@executable_path/../lib
// Run (the loader takes the driver from OCL_ICD_FILENAMES):
//   OCL_ICD_FILENAMES=$PWD/lib/libigdrcl.dylib DYLD_LIBRARY_PATH=$PWD/lib ./bin/cl_hello
#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void step(const char *name, int ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok)
		failures++;
}

#define CHECK(call)                                                                                                    \
	do {                                                                                                           \
		cl_int e_ = (call);                                                                                    \
		if (e_ != CL_SUCCESS) {                                                                                \
			printf("FAIL %s:%d %s = %d\n", __FILE__, __LINE__, #call, e_);                             \
			return 1;                                                                                      \
		}                                                                                                      \
	} while (0)

static void *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *p = n > 0 ? malloc((size_t)n) : NULL;
	if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
		free(p);
		p = NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return p;
}

int main(int argc, char **argv)
{
	const int from_source = argc > 2 && (!strcmp(argv[1], "source") || !strcmp(argv[1], "source-build"));
	const int build_only = argc > 2 && !strcmp(argv[1], "source-build");
	const char *kernel_path = argc > 2 && (!strcmp(argv[1], "kernel") || from_source) ? argv[2] : NULL;
	const char *kernel_name = argc > 3 ? argv[3] : "fill_ids";
	const int lifetime_only = argc > 1 && !strcmp(argv[1], "lifetime");
	setvbuf(stdout, NULL, _IONBF, 0);

	cl_uint np = 0;
	cl_int err = clGetPlatformIDs(0, NULL, &np);
	step("clGetPlatformIDs", err == CL_SUCCESS && np > 0);
	if (err != CL_SUCCESS || !np) {
		printf("  error %d, %u platforms (OCL_ICD_FILENAMES set? OCL_ICD_ENABLE_TRACE=1 shows the loader's search)\n",
		       err, np);
		return 1;
	}
	cl_platform_id platform;
	CHECK(clGetPlatformIDs(1, &platform, NULL));
	char buf[256];
	CHECK(clGetPlatformInfo(platform, CL_PLATFORM_NAME, sizeof(buf), buf, NULL));
	printf("platform: %s", buf);
	CHECK(clGetPlatformInfo(platform, CL_PLATFORM_VERSION, sizeof(buf), buf, NULL));
	printf(", %s\n", buf);

	cl_device_id dev;
	CHECK(clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &dev, NULL));
	cl_uint cus = 0;
	cl_ulong mem = 0, maxalloc = 0;
	CHECK(clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(buf), buf, NULL));
	CHECK(clGetDeviceInfo(dev, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cus), &cus, NULL));
	CHECK(clGetDeviceInfo(dev, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(mem), &mem, NULL));
	CHECK(clGetDeviceInfo(dev, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(maxalloc), &maxalloc, NULL));
	printf("device: %s, %u compute units, %llu MB global, max alloc %llu MB\n", buf, cus,
	       (unsigned long long)(mem >> 20), (unsigned long long)(maxalloc >> 20));
	CHECK(clGetDeviceInfo(dev, CL_DEVICE_VERSION, sizeof(buf), buf, NULL));
	printf("  %s", buf);
	CHECK(clGetDeviceInfo(dev, CL_DRIVER_VERSION, sizeof(buf), buf, NULL));
	printf(", driver %s\n", buf);
	step("device info", 1);

	cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
	CHECK(err);
	cl_command_queue q = clCreateCommandQueueWithProperties(ctx, dev, NULL, &err);
	CHECK(err);
	step("context and queue", 1);

	if (lifetime_only) {
		const size_t sizes[] = {4096, 16384, 4 << 20}; // within the small-buffers pool's threshold, and above it
		cl_mem m[3];
		for (int i = 0; i < 3; i++) {
			m[i] = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizes[i], NULL, &err);
			CHECK(err);
		}
		for (int i = 0; i < 3; i++)
			CHECK(clReleaseMemObject(m[i]));
		step("buffers created and released (no GPU work)", 1);
		CHECK(clReleaseCommandQueue(q));
		CHECK(clReleaseContext(ctx));
		step("queue and context released", 1);
		printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
		return failures ? 1 : 0;
	}

	if (!kernel_path) {
		const size_t size = 4 << 20;
		uint32_t *src = malloc(size), *dst = calloc(1, size);
		for (size_t i = 0; i < size / 4; i++)
			src[i] = (uint32_t)(i * 2654435761u);
		cl_mem b = clCreateBuffer(ctx, CL_MEM_READ_WRITE, size, NULL, &err);
		CHECK(err);
		CHECK(clEnqueueWriteBuffer(q, b, CL_TRUE, 0, size, src, 0, NULL, NULL));
		CHECK(clEnqueueReadBuffer(q, b, CL_TRUE, 0, size, dst, 0, NULL, NULL));
		step("buffer write + read back", memcmp(src, dst, size) == 0);
		clReleaseMemObject(b);
		free(dst);
		free(src);
	} else {
		size_t size = 0;
		unsigned char *bin = read_file(kernel_path, &size);
		if (!bin) {
			printf("FAIL cannot read %s\n", kernel_path);
			return 1;
		}
		cl_int status = 0;
		cl_program prog;
		if (from_source) {
			const char *text = (const char *)bin;
			prog = clCreateProgramWithSource(ctx, 1, &text, &size, &err);
		} else {
			prog = clCreateProgramWithBinary(ctx, 1, &dev, &size, (const unsigned char **)&bin, &status, &err);
		}
		CHECK(err);
		err = clBuildProgram(prog, 1, &dev, "", NULL, NULL);
		if (err != CL_SUCCESS || from_source) {
			char log[4096] = "";
			clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof(log) - 1, log, NULL);
			if (log[0])
				printf("  build log: %s\n", log);
		}
		CHECK(err);
		if (from_source) {
			size_t binsize = 0;
			CHECK(clGetProgramInfo(prog, CL_PROGRAM_BINARY_SIZES, sizeof(binsize), &binsize, NULL));
			printf("  program binary: %zu bytes\n", binsize);
			step("program built from OpenCL C source", binsize > 0);
		}
		cl_kernel k = clCreateKernel(prog, kernel_name, &err);
		CHECK(err);
		if (build_only) {
			size_t wg = 0;
			CHECK(clGetKernelWorkGroupInfo(k, dev, CL_KERNEL_WORK_GROUP_SIZE, sizeof(wg), &wg, NULL));
			printf("  kernel %s: max work-group size %zu\n", kernel_name, wg);
			step("kernel created", wg > 0);
			clReleaseKernel(k);
			clReleaseProgram(prog);
			free(bin);
			clReleaseCommandQueue(q);
			clReleaseContext(ctx);
			printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
			return failures ? 1 : 0;
		}
		const size_t n = 4096;
		cl_mem out = clCreateBuffer(ctx, CL_MEM_READ_WRITE, n * 4, NULL, &err);
		CHECK(err);
		CHECK(clSetKernelArg(k, 0, sizeof(out), &out));
		CHECK(clEnqueueNDRangeKernel(q, k, 1, NULL, &n, NULL, 0, NULL, NULL));
		uint32_t *res = calloc(n, 4);
		CHECK(clEnqueueReadBuffer(q, out, CL_TRUE, 0, n * 4, res, 0, NULL, NULL));
		int ok = 1;
		for (uint32_t i = 0; i < n; i++)
			if (res[i] != i * 3 + 1) {
				printf("  out[%u] = %u, expected %u\n", i, res[i], i * 3 + 1);
				ok = 0;
				break;
			}
		step(from_source ? "kernel from OpenCL C source" : "kernel from native binary", ok);
		free(res);
		clReleaseMemObject(out);
		clReleaseKernel(k);
		clReleaseProgram(prog);
		free(bin);
	}

	clReleaseCommandQueue(q);
	clReleaseContext(ctx);
	printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
