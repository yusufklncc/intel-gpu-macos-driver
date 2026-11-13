// SPDX-License-Identifier: MIT
// sycl_libs' SPIR-V kernel (kernel_compiler, source_language::spirv): compiled to share/rtc_kernels.spv by
// compute/build-sycl.sh stage with the DPC++ clang (OpenCL C 2.0, spir64) and llvm-spirv.
__kernel void rtc_spv_scale(__global const int *in, __global int *out, int k)
{
	size_t i = get_global_id(0);
	out[i] = in[i] * k + 1;
}
