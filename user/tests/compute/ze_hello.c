// SPDX-License-Identifier: MIT
// ze_hello: Intel's Level Zero driver (compute-runtime/NEO, libze_intel_gpu) on macOS over IntelXe.kext, through the
// Level Zero loader. No Mesa, no Metal. Steps, each printed with PASS/FAIL; the first failure ends the run:
//   1. zeInitDrivers (GPU) -> drivers -> devices: name, PCI id, EU/thread layout, memory, command queue groups
//   2. a context; USM allocations: device (VRAM), host (system memory the GPU reaches), shared
//   3. copy engine: an immediate synchronous command list on the copy group fills a host buffer with a pattern and
//      copies host -> device -> host; the result is checked (blitter only: XY_FAST_COLOR_BLT / MEM_COPY, no kernels)
//   4. copy engine again through a regular command list, a command queue and a fence (the submission path with
//      zeCommandQueueExecuteCommandLists)
// Steps 3-4 run without arguments; with one of the arguments below, its steps run instead:
//   5. "compute": a copy (queue + fence) and a fill + copy (immediate) on the compute group. NEO does both with its
//      built-in kernels, which need IGC at run time or offline-compiled built-ins in the working directory
//      (compute/ocloc-docker.sh builtins); without either, module creation fails
//   6. "kernel <file.bin> [name]": loads a native binary (zebin, e.g. from ocloc -device bmg on Linux) and runs its
//      kernel `name` (default fill_ids: out[i] = i * 3 + 1 for a global size of 4096) on the compute group, checks
//      the output
//   "init": stop after step 2
// Exit status 0 when every step that ran passed.
//
// Build (the build Mac; L0 = the prefix compute/build-neo.sh installs to):
//   clang -arch x86_64 -O1 -I$L0/include -o ze_hello ze_hello.c -L$L0/lib -lze_loader -Wl,-rpath,@executable_path/../lib
// Run (the test machine; the loader finds libze_intel_gpu.1.dylib through DYLD_LIBRARY_PATH or ZE_ENABLE_ALT_DRIVERS):
//   DYLD_LIBRARY_PATH=$PWD/lib ./bin/ze_hello
//   XEMAC_FAKE=1 ... init: xemac's canned Battlemage, no kext (any Mac, Rosetta included). No batch runs there, so
//   only "init" ends; the other steps wait for a completion that never comes.
#include <level_zero/ze_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(call)                                                                                                    \
	do {                                                                                                           \
		ze_result_t r_ = (call);                                                                               \
		if (r_ != ZE_RESULT_SUCCESS) {                                                                         \
			printf("FAIL %s:%d %s = 0x%x\n", __FILE__, __LINE__, #call, (unsigned)r_);                 \
			return -1;                                                                                     \
		}                                                                                                      \
	} while (0)

static void step(const char *name, int ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok)
		failures++;
}

static const char *group_flags(ze_command_queue_group_property_flags_t f, char *buf, size_t n)
{
	snprintf(buf, n, "%s%s%s%s", f & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE ? "compute " : "",
		 f & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY ? "copy " : "",
		 f & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COOPERATIVE_KERNELS ? "cooperative " : "",
		 f & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_METRICS ? "metrics" : "");
	return buf;
}

// The first group with `want` and, for copy, without compute (the pure blitter group).
static int find_group(ze_device_handle_t dev, ze_command_queue_group_property_flags_t want, uint32_t *ordinal)
{
	uint32_t n = 0;
	zeDeviceGetCommandQueueGroupProperties(dev, &n, NULL);
	ze_command_queue_group_properties_t props[16];
	if (n > 16)
		n = 16;
	for (uint32_t i = 0; i < n; i++)
		props[i] = (ze_command_queue_group_properties_t){.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_GROUP_PROPERTIES};
	zeDeviceGetCommandQueueGroupProperties(dev, &n, props);
	for (uint32_t i = 0; i < n; i++) {
		int compute = (props[i].flags & ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE) != 0;
		if (!(props[i].flags & want))
			continue;
		if (want == ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY && compute)
			continue;
		*ordinal = i;
		return 0;
	}
	return -1;
}

static int print_device(ze_device_handle_t dev)
{
	ze_device_properties_t p = {.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES};
	CHECK(zeDeviceGetProperties(dev, &p));
	printf("device: %s, vendor 0x%04x device 0x%04x, %u slices x %u subslices x %u EUs x %u threads, SIMD %u, "
	       "core clock %u MHz, max alloc %llu MB\n",
	       p.name, p.vendorId, p.deviceId, p.numSlices, p.numSubslicesPerSlice, p.numEUsPerSubslice, p.numThreadsPerEU,
	       p.physicalEUSimdWidth, p.coreClockRate, (unsigned long long)(p.maxMemAllocSize >> 20));
	uint32_t n = 0;
	CHECK(zeDeviceGetMemoryProperties(dev, &n, NULL));
	ze_device_memory_properties_t mem[4];
	if (n > 4)
		n = 4;
	for (uint32_t i = 0; i < n; i++)
		mem[i] = (ze_device_memory_properties_t){.stype = ZE_STRUCTURE_TYPE_DEVICE_MEMORY_PROPERTIES};
	CHECK(zeDeviceGetMemoryProperties(dev, &n, mem));
	for (uint32_t i = 0; i < n; i++)
		printf("memory %u: %s %llu MB, %u MHz, bus %u bits\n", i, mem[i].name,
		       (unsigned long long)(mem[i].totalSize >> 20), mem[i].maxClockRate, mem[i].maxBusWidth);
	n = 0;
	CHECK(zeDeviceGetCommandQueueGroupProperties(dev, &n, NULL));
	ze_command_queue_group_properties_t g[16];
	if (n > 16)
		n = 16;
	for (uint32_t i = 0; i < n; i++)
		g[i] = (ze_command_queue_group_properties_t){.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_GROUP_PROPERTIES};
	CHECK(zeDeviceGetCommandQueueGroupProperties(dev, &n, g));
	for (uint32_t i = 0; i < n; i++) {
		char buf[64];
		printf("queue group %u: %u queues, %s\n", i, g[i].numQueues, group_flags(g[i].flags, buf, sizeof(buf)));
	}
	return 0;
}

static int check_bytes(const uint8_t *p, size_t n, uint8_t v, const char *what)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != v) {
			printf("  %s: byte %zu is 0x%02x, expected 0x%02x\n", what, i, p[i], v);
			return 0;
		}
	return 1;
}

// fill(host) with a pattern, host -> device, device -> host2: immediate, synchronous.
static int copy_immediate(ze_context_handle_t ctx, ze_device_handle_t dev, uint32_t ordinal, size_t size, uint8_t pattern,
			  int *ok)
{
	ze_command_queue_desc_t qd = {.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC, .ordinal = ordinal,
				      .mode = ZE_COMMAND_QUEUE_MODE_SYNCHRONOUS};
	ze_command_list_handle_t cl;
	CHECK(zeCommandListCreateImmediate(ctx, dev, &qd, &cl));
	ze_host_mem_alloc_desc_t hd = {.stype = ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC};
	ze_device_mem_alloc_desc_t dd = {.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC};
	void *host, *host2, *devmem;
	CHECK(zeMemAllocHost(ctx, &hd, size, 4096, &host));
	CHECK(zeMemAllocHost(ctx, &hd, size, 4096, &host2));
	CHECK(zeMemAllocDevice(ctx, &dd, size, 4096, dev, &devmem));
	memset(host, 0, size);
	memset(host2, 0, size);
	CHECK(zeCommandListAppendMemoryFill(cl, host, &pattern, 1, size, NULL, 0, NULL));
	int fill_ok = check_bytes(host, size, pattern, "fill");
	CHECK(zeCommandListAppendMemoryCopy(cl, devmem, host, size, NULL, 0, NULL));
	CHECK(zeCommandListAppendMemoryCopy(cl, host2, devmem, size, NULL, 0, NULL));
	*ok = fill_ok && check_bytes(host2, size, pattern, "copy back");
	zeMemFree(ctx, devmem);
	zeMemFree(ctx, host2);
	zeMemFree(ctx, host);
	zeCommandListDestroy(cl);
	return 0;
}

// host -> device -> host through a regular command list, executed on a queue, waited on with a fence.
static int copy_queue(ze_context_handle_t ctx, ze_device_handle_t dev, uint32_t ordinal, size_t size, int *ok)
{
	ze_command_queue_desc_t qd = {.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC, .ordinal = ordinal,
				      .mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS};
	ze_command_queue_handle_t q;
	CHECK(zeCommandQueueCreate(ctx, dev, &qd, &q));
	ze_command_list_desc_t ld = {.stype = ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC, .commandQueueGroupOrdinal = ordinal};
	ze_command_list_handle_t cl;
	CHECK(zeCommandListCreate(ctx, dev, &ld, &cl));
	ze_host_mem_alloc_desc_t hd = {.stype = ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC};
	ze_device_mem_alloc_desc_t dd = {.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC};
	uint32_t *src, *dst;
	void *devmem;
	CHECK(zeMemAllocHost(ctx, &hd, size, 4096, (void **)&src));
	CHECK(zeMemAllocHost(ctx, &hd, size, 4096, (void **)&dst));
	CHECK(zeMemAllocDevice(ctx, &dd, size, 4096, dev, &devmem));
	for (size_t i = 0; i < size / 4; i++)
		src[i] = (uint32_t)(i * 2654435761u);
	memset(dst, 0, size);
	CHECK(zeCommandListAppendMemoryCopy(cl, devmem, src, size, NULL, 0, NULL));
	CHECK(zeCommandListAppendBarrier(cl, NULL, 0, NULL));
	CHECK(zeCommandListAppendMemoryCopy(cl, dst, devmem, size, NULL, 0, NULL));
	CHECK(zeCommandListClose(cl));
	ze_fence_desc_t fd = {.stype = ZE_STRUCTURE_TYPE_FENCE_DESC};
	ze_fence_handle_t fence;
	CHECK(zeFenceCreate(q, &fd, &fence));
	CHECK(zeCommandQueueExecuteCommandLists(q, 1, &cl, fence));
	CHECK(zeFenceHostSynchronize(fence, 5000000000ull)); // 5 s
	*ok = memcmp(src, dst, size) == 0;
	if (!*ok)
		for (size_t i = 0; i < size / 4; i++)
			if (src[i] != dst[i]) {
				printf("  word %zu is 0x%08x, expected 0x%08x\n", i, dst[i], src[i]);
				break;
			}
	zeFenceDestroy(fence);
	zeMemFree(ctx, devmem);
	zeMemFree(ctx, dst);
	zeMemFree(ctx, src);
	zeCommandListDestroy(cl);
	zeCommandQueueDestroy(q);
	return 0;
}

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

// out[i] = i * 3 + 1 from a native binary's kernel(global uint *out).
static int run_kernel(ze_context_handle_t ctx, ze_device_handle_t dev, uint32_t ordinal, const char *path, const char *name,
		      int *ok)
{
	size_t size = 0;
	void *bin = read_file(path, &size);
	if (!bin) {
		printf("FAIL cannot read %s\n", path);
		return -1;
	}
	ze_module_desc_t md = {.stype = ZE_STRUCTURE_TYPE_MODULE_DESC, .format = ZE_MODULE_FORMAT_NATIVE,
			       .inputSize = size, .pInputModule = bin};
	ze_module_handle_t mod;
	ze_module_build_log_handle_t log = NULL;
	ze_result_t r = zeModuleCreate(ctx, dev, &md, &mod, &log);
	if (r != ZE_RESULT_SUCCESS) {
		size_t ls = 0;
		if (log && zeModuleBuildLogGetString(log, &ls, NULL) == ZE_RESULT_SUCCESS && ls) {
			char *s = malloc(ls);
			zeModuleBuildLogGetString(log, &ls, s);
			printf("  build log: %s\n", s);
			free(s);
		}
		printf("FAIL zeModuleCreate = 0x%x\n", (unsigned)r);
		free(bin);
		return -1;
	}
	if (log)
		zeModuleBuildLogDestroy(log);
	ze_kernel_desc_t kd = {.stype = ZE_STRUCTURE_TYPE_KERNEL_DESC, .pKernelName = name};
	ze_kernel_handle_t k;
	CHECK(zeKernelCreate(mod, &kd, &k));
	const uint32_t n = 4096;
	uint32_t gx = 0, gy = 0, gz = 0;
	CHECK(zeKernelSuggestGroupSize(k, n, 1, 1, &gx, &gy, &gz));
	CHECK(zeKernelSetGroupSize(k, gx, gy, gz));
	ze_device_mem_alloc_desc_t dd = {.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC};
	ze_host_mem_alloc_desc_t hd = {.stype = ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC};
	uint32_t *out;
	CHECK(zeMemAllocShared(ctx, &dd, &hd, n * 4, 4096, dev, (void **)&out));
	memset(out, 0, n * 4);
	CHECK(zeKernelSetArgumentValue(k, 0, sizeof(out), &out));
	ze_command_queue_desc_t qd = {.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC, .ordinal = ordinal,
				      .mode = ZE_COMMAND_QUEUE_MODE_SYNCHRONOUS};
	ze_command_list_handle_t cl;
	CHECK(zeCommandListCreateImmediate(ctx, dev, &qd, &cl));
	ze_group_count_t groups = {n / gx, 1, 1};
	printf("  kernel %s: group size %u, %u groups\n", name, gx, groups.groupCountX);
	CHECK(zeCommandListAppendLaunchKernel(cl, k, &groups, NULL, 0, NULL));
	CHECK(zeCommandListHostSynchronize(cl, 5000000000ull));
	*ok = 1;
	for (uint32_t i = 0; i < n; i++)
		if (out[i] != i * 3 + 1) {
			printf("  out[%u] = %u, expected %u\n", i, out[i], i * 3 + 1);
			*ok = 0;
			break;
		}
	zeCommandListDestroy(cl);
	zeMemFree(ctx, out);
	zeKernelDestroy(k);
	zeModuleDestroy(mod);
	free(bin);
	return 0;
}

int main(int argc, char **argv)
{
	int want_compute = argc > 1 && !strcmp(argv[1], "compute");
	int init_only = argc > 1 && !strcmp(argv[1], "init");
	const char *kernel_path = argc > 2 && !strcmp(argv[1], "kernel") ? argv[2] : NULL;
	const char *kernel_name = argc > 3 ? argv[3] : "fill_ids";
	setvbuf(stdout, NULL, _IONBF, 0);

	ze_init_driver_type_desc_t init = {.stype = ZE_STRUCTURE_TYPE_INIT_DRIVER_TYPE_DESC,
					   .flags = ZE_INIT_DRIVER_TYPE_FLAG_GPU};
	uint32_t ndrv = 0;
	ze_result_t r = zeInitDrivers(&ndrv, NULL, &init);
	step("zeInitDrivers", r == ZE_RESULT_SUCCESS && ndrv > 0);
	if (r != ZE_RESULT_SUCCESS || !ndrv) {
		printf("  result 0x%x, %u drivers (ZE_ENABLE_LOADER_DEBUG_TRACE=1 shows what the loader tried)\n", (unsigned)r,
		       ndrv);
		return 1;
	}
	ze_driver_handle_t drv;
	ndrv = 1;
	CHECK(zeInitDrivers(&ndrv, &drv, &init));
	ze_driver_properties_t dp = {.stype = ZE_STRUCTURE_TYPE_DRIVER_PROPERTIES};
	CHECK(zeDriverGetProperties(drv, &dp));
	ze_api_version_t api;
	CHECK(zeDriverGetApiVersion(drv, &api));
	printf("driver version 0x%x, API %u.%u\n", dp.driverVersion, ZE_MAJOR_VERSION(api), ZE_MINOR_VERSION(api));

	uint32_t ndev = 0;
	CHECK(zeDeviceGet(drv, &ndev, NULL));
	step("zeDeviceGet", ndev > 0);
	if (!ndev)
		return 1;
	ze_device_handle_t dev;
	ndev = 1;
	CHECK(zeDeviceGet(drv, &ndev, &dev));
	if (print_device(dev))
		return 1;
	step("device properties", 1);

	ze_context_desc_t cd = {.stype = ZE_STRUCTURE_TYPE_CONTEXT_DESC};
	ze_context_handle_t ctx;
	CHECK(zeContextCreate(drv, &cd, &ctx));
	step("zeContextCreate", 1);

	// USM: one of each kind, written by the CPU where it can be
	{
		ze_device_mem_alloc_desc_t dd = {.stype = ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC};
		ze_host_mem_alloc_desc_t hd = {.stype = ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC};
		void *d = NULL, *h = NULL, *s = NULL;
		CHECK(zeMemAllocDevice(ctx, &dd, 1 << 20, 4096, dev, &d));
		CHECK(zeMemAllocHost(ctx, &hd, 1 << 20, 4096, &h));
		CHECK(zeMemAllocShared(ctx, &dd, &hd, 1 << 20, 4096, dev, &s));
		memset(h, 0x11, 1 << 20);
		memset(s, 0x22, 1 << 20);
		printf("  device %p, host %p, shared %p\n", d, h, s);
		zeMemFree(ctx, s);
		zeMemFree(ctx, h);
		zeMemFree(ctx, d);
		step("USM device/host/shared alloc", 1);
	}
	if (init_only) {
		zeContextDestroy(ctx);
		printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
		return failures ? 1 : 0;
	}

	uint32_t copy_ord = 0, compute_ord = 0;
	if (want_compute || kernel_path) {
		// the compute-engine steps alone, so that each run tests one thing
	} else if (find_group(dev, ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COPY, &copy_ord) == 0) {
		int ok = 0;
		if (copy_immediate(ctx, dev, copy_ord, 1 << 20, 0xa5, &ok))
			return 1;
		step("copy engine: fill + copy (immediate)", ok);
		if (copy_queue(ctx, dev, copy_ord, 4 << 20, &ok))
			return 1;
		step("copy engine: copy (queue + fence)", ok);
	} else {
		printf("SKIP no copy-only queue group\n");
	}

	if (find_group(dev, ZE_COMMAND_QUEUE_GROUP_PROPERTY_FLAG_COMPUTE, &compute_ord) == 0) {
		if (want_compute) {
			int ok = 0;
			if (copy_queue(ctx, dev, compute_ord, 4 << 20, &ok))
				return 1;
			step("compute engine: copy (queue + fence)", ok);
			if (copy_immediate(ctx, dev, compute_ord, 1 << 20, 0x5a, &ok))
				return 1; // fill: built-in kernels
			step("compute engine: fill + copy (immediate, built-in kernels)", ok);
		}
		if (kernel_path) {
			int ok = 0;
			if (run_kernel(ctx, dev, compute_ord, kernel_path, kernel_name, &ok))
				return 1;
			step("kernel from native binary", ok);
		}
	}

	zeContextDestroy(ctx);
	printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
