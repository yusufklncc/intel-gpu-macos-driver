// SPDX-License-Identifier: MIT
// ze_hello's kernel test: out[i] = i * 3 + 1. Compiled offline for Battlemage with Intel's ocloc on Linux
// (compute/ocloc-docker.sh) and loaded as a native binary (ZE_MODULE_FORMAT_NATIVE): no compiler on the Mac.
kernel void fill_ids(global uint *out)
{
	size_t i = get_global_id(0);
	out[i] = (uint)i * 3u + 1u;
}
