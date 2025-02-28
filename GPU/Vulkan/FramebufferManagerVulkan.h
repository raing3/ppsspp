// Copyright (c) 2015- PPSSPP Project.

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

#include "Common/GPU/Vulkan/VulkanLoader.h"
#include "GPU/GPUInterface.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "GPU/Common/GPUDebugInterface.h"
#include "GPU/Common/PresentationCommon.h"
#include "GPU/Vulkan/VulkanUtil.h"

class TextureCacheVulkan;
class DrawEngineVulkan;
class VulkanContext;
class ShaderManagerVulkan;
class VulkanTexture;
class VulkanPushBuffer;

class FramebufferManagerVulkan : public FramebufferManagerCommon {
public:
	FramebufferManagerVulkan(Draw::DrawContext *draw);
	~FramebufferManagerVulkan();

	void BeginFrameVulkan();  // there's a BeginFrame in the base class, which this calls
	void EndFrame();

	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;

	// If within a render pass, this will just issue a regular clear. If beginning a new render pass,
	// do that.
	void NotifyClear(bool clearColor, bool clearAlpha, bool clearDepth, uint32_t color, float depth);
};
