// Copyright (c) 2016- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <tuple>
#include <map>

#include "Common/Data/Collections/Hashmaps.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanImage.h"
#include "Common/GPU/Vulkan/VulkanLoader.h"
#include "Common/GPU/Vulkan/VulkanMemory.h"

extern const VkComponentMapping VULKAN_4444_SWIZZLE;
extern const VkComponentMapping VULKAN_1555_SWIZZLE;
extern const VkComponentMapping VULKAN_565_SWIZZLE;
extern const VkComponentMapping VULKAN_8888_SWIZZLE;

// Note: some drivers prefer B4G4R4A4_UNORM_PACK16 over R4G4B4A4_UNORM_PACK16.
#define VULKAN_4444_FORMAT VK_FORMAT_B4G4R4A4_UNORM_PACK16
#define VULKAN_1555_FORMAT VK_FORMAT_A1R5G5B5_UNORM_PACK16
#define VULKAN_565_FORMAT  VK_FORMAT_B5G6R5_UNORM_PACK16   // TODO: Does not actually have mandatory support, though R5G6B5 does! See #14602
#define VULKAN_8888_FORMAT VK_FORMAT_R8G8B8A8_UNORM

// Manager for compute shaders that upload things (and those have two bindings: a storage buffer to read from and an image to write to).
class VulkanComputeShaderManager {
public:
	VulkanComputeShaderManager(VulkanContext *vulkan);
	~VulkanComputeShaderManager();

	void DeviceLost() {
		DestroyDeviceObjects();
	}
	void DeviceRestore(VulkanContext *vulkan) {
		vulkan_ = vulkan;
		InitDeviceObjects();
	}

	// Note: This doesn't cache. The descriptor is for immediate use only.
	VkDescriptorSet GetDescriptorSet(VkImageView image, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range, VkBuffer buffer2 = VK_NULL_HANDLE, VkDeviceSize offset2 = 0, VkDeviceSize range2 = 0);

	// This of course caches though.
	VkPipeline GetPipeline(VkShaderModule cs);
	VkPipelineLayout GetPipelineLayout() const { return pipelineLayout_; }

	void BeginFrame();
	void EndFrame();

private:
	void InitDeviceObjects();
	void DestroyDeviceObjects();

	VulkanContext *vulkan_ = nullptr;
	VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
	VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;

	struct FrameData {
		FrameData() : descPool("VulkanComputeShaderManager", true) {
			descPool.Setup([this] { });
		}

		VulkanDescSetPool descPool;
	};
	FrameData frameData_[VulkanContext::MAX_INFLIGHT_FRAMES];

	struct PipelineKey {
		VkShaderModule module;
	};
	
	DenseHashMap<PipelineKey, VkPipeline, (VkPipeline)VK_NULL_HANDLE> pipelines_;
};


VkShaderModule CompileShaderModule(VulkanContext *vulkan, VkShaderStageFlagBits stage, const char *code, std::string *error);
