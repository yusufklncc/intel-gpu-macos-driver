// vk_dbuf_empty_set: ANV's EXT_descriptor_buffer offsets after a set layout without bindings (issue #9, mesa 0109).
// SPDX-License-Identifier: MIT
//
// anv_cmd_buffer_set_descriptor_buffer_offsets() flags only the stages of the set layout it is given. A set layout
// without bindings has no stages, so a new offset given with it is stored in the bind state but no stage is flagged
// and genX(flush_binding_mode) does not reload push_constants.buffer.sets[] (the per-set offsets shaders read from
// push constants). A later call with a set layout that has stages and the *same* offset is "no change", so the shader
// keeps reading the set at the offset of the call before. The Metal plugin's descriptor-buffer model did exactly that
// (an empty stage plan got the page cursor as its offset, the next real region started there): Safari's icon gradient
// in RenderBox read an older region's descriptors when table-pc converted both stages (issue #9).
//
// One compute command buffer, one descriptor buffer with two regions of set layout S (src + dst storage buffers):
// R1: src = X (holds 111), R2: src = Y (holds 222); both dst = the result buffer.
//   1. offsets(layout B: set 0 = S, R1), push slot 0, dispatch      -> result[0] = 111
//   2. offsets(layout A: set 0 = an empty set layout, R2)            (no dispatch)
//   3. offsets(layout B, R2), push slot 1, dispatch                  -> result[1] must be 222 (ANV without 0109: 111)
//   4. offsets(layout B, R1), push slot 2, dispatch                  -> result[2] = 111 (control: a real change)
//   5. offsets(layout B, R2), push slot 3, dispatch                  -> result[3] = 222 (control)
// The graphics bind point has the same code path (the stage of an empty layout is none of VS/FS); compute is the
// smallest way to show it. Exit status 0 = right, 1 = wrong values, 2 = setup failure.
//
//   build: user/tests/vulkan/build.sh
//   run:   user/tests/vulkan/vk_dbuf_empty_set [path/to/libvulkan_intel.dylib]
//
// The shader (glslangValidator -V --target-env vulkan1.2):
//   #version 450
//   layout(local_size_x = 1) in;
//   layout(set = 0, binding = 0) readonly buffer Src { uint v; } src;
//   layout(set = 0, binding = 1) buffer Dst { uint r[]; } dst;
//   layout(push_constant) uniform PC { uint slot; } pc;
//   void main() { dst.r[pc.slot] = src.v; }

#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

static const uint32_t b_spv[] = {
	0x07230203,0x00010500,0x0008000b,0x0000001d,0x00000000,0x00020011,0x00000001,0x0006000b,
	0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
	0x0008000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x0000000a,0x0000000f,0x00000015,
	0x00060010,0x00000004,0x00000011,0x00000001,0x00000001,0x00000001,0x00030003,0x00000002,
	0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00030005,0x00000008,0x00747344,
	0x00040006,0x00000008,0x00000000,0x00000072,0x00030005,0x0000000a,0x00747364,0x00030005,
	0x0000000d,0x00004350,0x00050006,0x0000000d,0x00000000,0x746f6c73,0x00000000,0x00030005,
	0x0000000f,0x00006370,0x00030005,0x00000013,0x00637253,0x00040006,0x00000013,0x00000000,
	0x00000076,0x00030005,0x00000015,0x00637273,0x00040047,0x00000007,0x00000006,0x00000004,
	0x00030047,0x00000008,0x00000002,0x00050048,0x00000008,0x00000000,0x00000023,0x00000000,
	0x00040047,0x0000000a,0x00000021,0x00000001,0x00040047,0x0000000a,0x00000022,0x00000000,
	0x00030047,0x0000000d,0x00000002,0x00050048,0x0000000d,0x00000000,0x00000023,0x00000000,
	0x00030047,0x00000013,0x00000002,0x00040048,0x00000013,0x00000000,0x00000018,0x00050048,
	0x00000013,0x00000000,0x00000023,0x00000000,0x00030047,0x00000015,0x00000018,0x00040047,
	0x00000015,0x00000021,0x00000000,0x00040047,0x00000015,0x00000022,0x00000000,0x00040047,
	0x0000001c,0x0000000b,0x00000019,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,
	0x00040015,0x00000006,0x00000020,0x00000000,0x0003001d,0x00000007,0x00000006,0x0003001e,
	0x00000008,0x00000007,0x00040020,0x00000009,0x0000000c,0x00000008,0x0004003b,0x00000009,
	0x0000000a,0x0000000c,0x00040015,0x0000000b,0x00000020,0x00000001,0x0004002b,0x0000000b,
	0x0000000c,0x00000000,0x0003001e,0x0000000d,0x00000006,0x00040020,0x0000000e,0x00000009,
	0x0000000d,0x0004003b,0x0000000e,0x0000000f,0x00000009,0x00040020,0x00000010,0x00000009,
	0x00000006,0x0003001e,0x00000013,0x00000006,0x00040020,0x00000014,0x0000000c,0x00000013,
	0x0004003b,0x00000014,0x00000015,0x0000000c,0x00040020,0x00000016,0x0000000c,0x00000006,
	0x00040017,0x0000001a,0x00000006,0x00000003,0x0004002b,0x00000006,0x0000001b,0x00000001,
	0x0006002c,0x0000001a,0x0000001c,0x0000001b,0x0000001b,0x0000001b,0x00050036,0x00000002,
	0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x00000010,0x00000011,
	0x0000000f,0x0000000c,0x0004003d,0x00000006,0x00000012,0x00000011,0x00050041,0x00000016,
	0x00000017,0x00000015,0x0000000c,0x0004003d,0x00000006,0x00000018,0x00000017,0x00060041,
	0x00000016,0x00000019,0x0000000a,0x0000000c,0x00000012,0x0003003e,0x00000019,0x00000018,
	0x000100fd,0x00010038
};

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkDevice device;

#define CHECK(call)                                                                       \
	do {                                                                              \
		VkResult r_ = (call);                                                     \
		if (r_ != VK_SUCCESS) {                                                   \
			printf("%s: VkResult %d\n", #call, r_);                           \
			exit(2);                                                          \
		}                                                                         \
	} while (0)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(device, #name)

typedef struct {
	VkBuffer buf;
	VkDeviceMemory mem;
	VkDeviceAddress addr;
	void *map;
} hbuf;

static VkPhysicalDeviceMemoryProperties mp;

// a host-visible, coherent, device-addressable buffer of `size` bytes, mapped
static hbuf make_buffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
	DFN(vkCreateBuffer);
	DFN(vkGetBufferMemoryRequirements);
	DFN(vkAllocateMemory);
	DFN(vkBindBufferMemory);
	DFN(vkMapMemory);
	DFN(vkGetBufferDeviceAddress);
	hbuf b = {0};
	VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
				  .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
	CHECK(vkCreateBuffer(device, &bci, NULL, &b.buf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, b.buf, &req);
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
		if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			type = i;
	if (type == UINT32_MAX) {
		printf("no host-visible coherent memory type in 0x%x\n", req.memoryTypeBits);
		exit(2);
	}
	VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
					.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
	VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fi,
				    .allocationSize = req.size, .memoryTypeIndex = type};
	CHECK(vkAllocateMemory(device, &mai, NULL, &b.mem));
	CHECK(vkBindBufferMemory(device, b.buf, b.mem, 0));
	CHECK(vkMapMemory(device, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
	memset(b.map, 0, size);
	VkBufferDeviceAddressInfo ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b.buf};
	b.addr = vkGetBufferDeviceAddress(device, &ai);
	return b;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "../../../mesa/build/src/intel/vulkan/libvulkan_intel.dylib";
	void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		printf("dlopen: %s\n", dlerror());
		return 2;
	}
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
	if (!gipa) {
		printf("no vk_icdGetInstanceProcAddr\n");
		return 2;
	}
	VkInstance instance = VK_NULL_HANDLE;
	PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_dbuf_empty_set",
				 .apiVersion = VK_API_VERSION_1_3};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	CHECK(vkCreateInstance(&ici, NULL, &instance));
	PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
		(PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
	PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 =
		(PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2");
	PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties =
		(PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties");
	PFN_vkCreateDevice vkCreateDevice = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
	gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");

	uint32_t n = 1;
	VkPhysicalDevice pd;
	CHECK(vkEnumeratePhysicalDevices(instance, &n, &pd));
	if (!n) {
		printf("no physical device\n");
		return 2;
	}
	VkPhysicalDeviceDescriptorBufferPropertiesEXT dbp = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
	VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dbp};
	vkGetPhysicalDeviceProperties2(pd, &p2);
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	printf("%s, descriptorBufferOffsetAlignment %" PRIu64 ", storageBufferDescriptorSize %zu\n",
	       p2.properties.deviceName, dbp.descriptorBufferOffsetAlignment, dbp.storageBufferDescriptorSize);

	VkPhysicalDeviceDescriptorBufferFeaturesEXT dbf = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, .descriptorBuffer = VK_TRUE};
	VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
						.pNext = &dbf, .bufferDeviceAddress = VK_TRUE};
	const char *ext[] = {VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME};
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
				       .queueCount = 1, .pQueuePriorities = &prio};
	VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f12, .queueCreateInfoCount = 1,
				  .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = ext};
	CHECK(vkCreateDevice(pd, &dci, NULL, &device));

	DFN(vkGetDeviceQueue);
	DFN(vkCreateDescriptorSetLayout);
	DFN(vkGetDescriptorSetLayoutSizeEXT);
	DFN(vkGetDescriptorSetLayoutBindingOffsetEXT);
	DFN(vkGetDescriptorEXT);
	DFN(vkCreatePipelineLayout);
	DFN(vkCreateShaderModule);
	DFN(vkCreateComputePipelines);
	DFN(vkCreateCommandPool);
	DFN(vkAllocateCommandBuffers);
	DFN(vkBeginCommandBuffer);
	DFN(vkEndCommandBuffer);
	DFN(vkCmdBindPipeline);
	DFN(vkCmdBindDescriptorBuffersEXT);
	DFN(vkCmdSetDescriptorBufferOffsetsEXT);
	DFN(vkCmdPushConstants);
	DFN(vkCmdDispatch);
	DFN(vkCmdPipelineBarrier);
	DFN(vkCreateFence);
	DFN(vkQueueSubmit);
	DFN(vkWaitForFences);
	DFN(vkDeviceWaitIdle);
	VkQueue queue;
	vkGetDeviceQueue(device, 0, 0, &queue);

	// set layout S (src, dst) and the empty set layout E, both for descriptor buffers
	const VkDescriptorSetLayoutBinding sb[2] = {
		{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
	VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
						.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
						.bindingCount = 2, .pBindings = sb};
	VkDescriptorSetLayout S, E;
	CHECK(vkCreateDescriptorSetLayout(device, &slci, NULL, &S));
	slci.bindingCount = 0;
	slci.pBindings = NULL;
	CHECK(vkCreateDescriptorSetLayout(device, &slci, NULL, &E));

	VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
	VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
					   .pSetLayouts = &S, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
	VkPipelineLayout LB, LA;
	CHECK(vkCreatePipelineLayout(device, &plci, NULL, &LB));
	plci.pSetLayouts = &E;
	CHECK(vkCreatePipelineLayout(device, &plci, NULL, &LA));

	VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof b_spv,
					 .pCode = b_spv};
	VkShaderModule sm;
	CHECK(vkCreateShaderModule(device, &smci, NULL, &sm));
	VkComputePipelineCreateInfo cpci = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
		.stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
			  .module = sm, .pName = "main"},
		.layout = LB};
	VkPipeline pipe;
	CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

	// buffers: X = 111, Y = 222, the result, and the descriptor buffer with regions R1 and R2
	hbuf X = make_buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	hbuf Y = make_buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	hbuf R = make_buffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	*(uint32_t *)X.map = 111;
	*(uint32_t *)Y.map = 222;
	VkDeviceSize ssize = 0, o0 = 0, o1 = 0;
	vkGetDescriptorSetLayoutSizeEXT(device, S, &ssize);
	vkGetDescriptorSetLayoutBindingOffsetEXT(device, S, 0, &o0);
	vkGetDescriptorSetLayoutBindingOffsetEXT(device, S, 1, &o1);
	const VkDeviceSize al = dbp.descriptorBufferOffsetAlignment ? dbp.descriptorBufferOffsetAlignment : 64;
	const VkDeviceSize stride = (ssize + al - 1) / al * al;
	hbuf D = make_buffer(4 * stride, VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT);
	const VkDeviceSize r1 = 0, r2 = stride;
	const struct { VkDeviceSize at; const hbuf *src; } region[2] = {{r1, &X}, {r2, &Y}};
	for (int k = 0; k < 2; k++) {
		VkDescriptorAddressInfoEXT a = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
						.address = region[k].src->addr, .range = 256};
		VkDescriptorGetInfoEXT gi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
					     .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .data.pStorageBuffer = &a};
		vkGetDescriptorEXT(device, &gi, dbp.storageBufferDescriptorSize, (uint8_t *)D.map + region[k].at + o0);
		a.address = R.addr;
		vkGetDescriptorEXT(device, &gi, dbp.storageBufferDescriptorSize, (uint8_t *)D.map + region[k].at + o1);
	}
	printf("set layout S: %" PRIu64 " bytes (src at %" PRIu64 ", dst at %" PRIu64 "), R1 at %" PRIu64
	       ", R2 at %" PRIu64 "\n", ssize, o0, o1, r1, r2);

	VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(device, &cpi, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
					    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(device, &cbai, &cb));
	VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	CHECK(vkBeginCommandBuffer(cb, &cbbi));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	VkDescriptorBufferBindingInfoEXT bi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
					       .address = D.addr,
					       .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT};
	vkCmdBindDescriptorBuffersEXT(cb, 1, &bi);
	const uint32_t zero = 0;
	struct { VkPipelineLayout layout; VkDeviceSize off; int32_t slot; } step[5] = {
		{LB, r1, 0}, {LA, r2, -1}, {LB, r2, 1}, {LB, r1, 2}, {LB, r2, 3}};
	for (int s = 0; s < 5; s++) {
		vkCmdSetDescriptorBufferOffsetsEXT(cb, VK_PIPELINE_BIND_POINT_COMPUTE, step[s].layout, 0, 1, &zero,
						   &step[s].off);
		if (step[s].slot < 0)
			continue;
		const uint32_t slot = (uint32_t)step[s].slot;
		vkCmdPushConstants(cb, LB, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &slot);
		vkCmdDispatch(cb, 1, 1, 1);
	}
	VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			      .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
			     NULL);
	CHECK(vkEndCommandBuffer(cb));
	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	CHECK(vkCreateFence(device, &fci, NULL, &fence));
	VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
	CHECK(vkQueueSubmit(queue, 1, &si, fence));
	CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
	CHECK(vkDeviceWaitIdle(device));

	const uint32_t *res = R.map, want[4] = {111, 222, 111, 222};
	const char *what[4] = {"R1", "R2 after an empty-layout call at R2", "R1 (control)", "R2 (control)"};
	int bad = 0;
	for (int k = 0; k < 4; k++) {
		printf("  result[%d] = %3u, want %u (%s)%s\n", k, res[k], want[k], what[k],
		       res[k] == want[k] ? "" : "  <-- WRONG");
		bad += res[k] != want[k];
	}
	printf(bad ? "FAIL: the set offset given with an empty set layout was not reloaded (mesa patch 0109)\n"
		   : "PASS\n");
	return bad ? 1 : 0;
}
