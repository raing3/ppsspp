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

#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "GPU/Vulkan/VulkanUtil.h"

using namespace PPSSPP_VK;

const VkComponentMapping VULKAN_4444_SWIZZLE = { VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B };
const VkComponentMapping VULKAN_1555_SWIZZLE = { VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A };
const VkComponentMapping VULKAN_565_SWIZZLE = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
const VkComponentMapping VULKAN_8888_SWIZZLE = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };

VkShaderModule CompileShaderModule(VulkanContext *vulkan, VkShaderStageFlagBits stage, const char *code, std::string *error) {
	std::vector<uint32_t> spirv;
	bool success = GLSLtoSPV(stage, code, GLSLVariant::VULKAN, spirv, error);
	if (!error->empty()) {
		if (success) {
			ERROR_LOG(G3D, "Warnings in shader compilation!");
		} else {
			ERROR_LOG(G3D, "Error in shader compilation!");
		}
		ERROR_LOG(G3D, "Messages: %s", error->c_str());
		ERROR_LOG(G3D, "Shader source:\n%s", LineNumberString(code).c_str());
		OutputDebugStringUTF8("Messages:\n");
		OutputDebugStringUTF8(error->c_str());
		OutputDebugStringUTF8(LineNumberString(code).c_str());
		return VK_NULL_HANDLE;
	} else {
		VkShaderModule module;
		if (vulkan->CreateShaderModule(spirv, &module)) {
			return module;
		} else {
			return VK_NULL_HANDLE;
		}
	}
}

VulkanComputeShaderManager::VulkanComputeShaderManager(VulkanContext *vulkan) : vulkan_(vulkan), pipelines_(8) {
}
VulkanComputeShaderManager::~VulkanComputeShaderManager() {}

void VulkanComputeShaderManager::InitDeviceObjects() {
	VkPipelineCacheCreateInfo pc{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	VkResult res = vkCreatePipelineCache(vulkan_->GetDevice(), &pc, nullptr, &pipelineCache_);
	_assert_(VK_SUCCESS == res);

	VkDescriptorSetLayoutBinding bindings[3] = {};
	bindings[0].descriptorCount = 1;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[0].binding = 0;
	bindings[1].descriptorCount = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1].binding = 1;
	bindings[2].descriptorCount = 1;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[2].binding = 2;

	VkDevice device = vulkan_->GetDevice();

	VkDescriptorSetLayoutCreateInfo dsl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dsl.bindingCount = ARRAY_SIZE(bindings);
	dsl.pBindings = bindings;
	res = vkCreateDescriptorSetLayout(device, &dsl, nullptr, &descriptorSetLayout_);
	_assert_(VK_SUCCESS == res);

	std::vector<VkDescriptorPoolSize> dpTypes;
	dpTypes.resize(2);
	dpTypes[0].descriptorCount = 8192;
	dpTypes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	dpTypes[1].descriptorCount = 4096;
	dpTypes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

	VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dp.flags = 0;   // Don't want to mess around with individually freeing these, let's go fixed each frame and zap the whole array. Might try the dynamic approach later.
	dp.maxSets = 4096;  // GTA can end up creating more than 1000 textures in the first frame!

	for (int i = 0; i < ARRAY_SIZE(frameData_); i++) {
		frameData_[i].descPool.Create(vulkan_, dp, dpTypes);
	}

	VkPushConstantRange push = {};
	push.offset = 0;
	push.size = 16;
	push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pl.pPushConstantRanges = &push;
	pl.pushConstantRangeCount = 1;
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &descriptorSetLayout_;
	pl.flags = 0;
	res = vkCreatePipelineLayout(device, &pl, nullptr, &pipelineLayout_);
	_assert_(VK_SUCCESS == res);
}

void VulkanComputeShaderManager::DestroyDeviceObjects() {
	for (int i = 0; i < ARRAY_SIZE(frameData_); i++) {
		frameData_[i].descPool.Destroy();
	}
	if (descriptorSetLayout_) {
		vulkan_->Delete().QueueDeleteDescriptorSetLayout(descriptorSetLayout_);
	}
	pipelines_.Iterate([&](const PipelineKey &key, VkPipeline pipeline) {
		vulkan_->Delete().QueueDeletePipeline(pipeline);
	});
	pipelines_.Clear();

	if (pipelineLayout_) {
		vulkan_->Delete().QueueDeletePipelineLayout(pipelineLayout_);
	}
	if (pipelineCache_ != VK_NULL_HANDLE) {
		vulkan_->Delete().QueueDeletePipelineCache(pipelineCache_);
	}
}

VkDescriptorSet VulkanComputeShaderManager::GetDescriptorSet(VkImageView image, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range, VkBuffer buffer2, VkDeviceSize offset2, VkDeviceSize range2) {
	int curFrame = vulkan_->GetCurFrame();
	FrameData &frameData = frameData_[curFrame];
	VkDescriptorSet desc = frameData.descPool.Allocate(1, &descriptorSetLayout_);
	_assert_(desc != VK_NULL_HANDLE);

	VkWriteDescriptorSet writes[2]{};
	int n = 0;
	VkDescriptorImageInfo imageInfo = {};
	VkDescriptorBufferInfo bufferInfo[2] = {};
	if (image) {
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		imageInfo.imageView = image;
		imageInfo.sampler = VK_NULL_HANDLE;
		writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[n].dstBinding = 0;
		writes[n].pImageInfo = &imageInfo;
		writes[n].descriptorCount = 1;
		writes[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[n].dstSet = desc;
		n++;
	}
	bufferInfo[0].buffer = buffer;
	bufferInfo[0].offset = offset;
	bufferInfo[0].range = range;
	writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[n].dstBinding = 1;
	writes[n].pBufferInfo = &bufferInfo[0];
	writes[n].descriptorCount = 1;
	writes[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[n].dstSet = desc;
	n++;
	if (buffer2) {
		bufferInfo[1].buffer = buffer2;
		bufferInfo[1].offset = offset2;
		bufferInfo[1].range = range2;
		writes[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[n].dstBinding = 2;
		writes[n].pBufferInfo = &bufferInfo[1];
		writes[n].descriptorCount = 1;
		writes[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[n].dstSet = desc;
		n++;
	}
	vkUpdateDescriptorSets(vulkan_->GetDevice(), n, writes, 0, nullptr);
	return desc;
}

VkPipeline VulkanComputeShaderManager::GetPipeline(VkShaderModule cs) {
	PipelineKey key{ cs };
	VkPipeline pipeline = pipelines_.Get(key);
	if (pipeline)
		return pipeline;

	VkComputePipelineCreateInfo pci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pci.stage.module = cs;
	pci.stage.pName = "main";
	pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pci.layout = pipelineLayout_;
	pci.flags = 0;

	VkResult res = vkCreateComputePipelines(vulkan_->GetDevice(), pipelineCache_, 1, &pci, nullptr, &pipeline);
	_assert_(res == VK_SUCCESS);

	pipelines_.Insert(key, pipeline);
	return pipeline;
}

void VulkanComputeShaderManager::BeginFrame() {
	int curFrame = vulkan_->GetCurFrame();
	FrameData &frame = frameData_[curFrame];
	frame.descPool.Reset();
}

void VulkanComputeShaderManager::EndFrame() {
}
